// cod3 - [acct2-bro] 2026-11-05 the frame's 2D (and a first try at its 3D) drawn natively by ngl_vk, beside the GPU
// emulation: steps 3 and 4 of dev/agents/acct2-bro/COD3-NGL-PLAN.md (owner 2026-11-05 09:11: "NGL renderer, BRO's
// way"). A check only: the game keeps drawing through the GPU emulation, and with cod3_debug_ngl_side off (the
// default) nothing here runs.
//
// With it on, every 10 s for the first 80 s (frames 600, 1200, ... 4800 counted from when it is turned on), at the
// start of nglListSend, the frame's NGL lists are read the way NGL's own back end walks them:
//   - scenes from the root (0x8290E9A8), each scene's children first (first child +0x2F8, next sibling +0x2F4);
//   - only scenes drawn to the 640x480 virtual screen (their color target, +0x328, has flag 0x2000 at +0x18; a scene
//     with no target counts too);
//   - per scene the opaque list (+0x334) in the order the quads came, then the blended list (+0x338) by its key,
//     largest first (Big Red One's order, std_InsertionSort_TranslucentNodes); both lists are pushed at their head, so
//     they are read backwards. Then a third list (+0x344, count +0x348) where 0x823F2DB0 puts the grass
//     (GrassRenderNode, vtable 0x8207B798; its Render, 0x823F79B0, calls 0x823F6DF0 with node +0x0C and +0x10).
//     A node: +0x00 its vtable (quads 0x82018020, strings 0x82018A20), +0x04 next, +0x08 key, +0x10 the quad's
//     0x68 bytes (four 0x14-byte vertices x, y, u, v, color; +0x60 the NGL texture).
//   - a string node (0x30 bytes, on the blended list): +0x0C the text, +0x10 the font, +0x14 x, +0x18 y, +0x1C z,
//     +0x20 / +0x24 the x and y scale, +0x28 the color; laid out the way Big Red One's nglBuildTextGlyphs does
//     (bro/src/ngl/ngl_font.c) from the font's glyph records (0x1C bytes a glyph: x, y, width, height, x and y
//     offsets, advance; CoD3's font layout is below, at kFontTexture).
// The quads and glyphs are drawn by ngl_vk (the library Big Red One's port draws everything with) into a 1280x720
// picture, the 640x480 screen stretched to it as the game does, saved as
// userdata/logs/screenshots/ngl-side-<frame>-vk.ppm, and the game's own screenshot of the same moment is asked for
// (report-*.png, named in the log). The first dump, and the first with a dozen quads or more, list every quad and
// string; each dump counts the other nodes in the lists (the 3D ones) by vtable.
//
// Textures are read from guest memory and decoded on the CPU, once each: the NGL texture's Direct3D texture (+0x14)
// carries a Xenos fetch constant at +0x1C (tiled bit and pitch, format and endianness, base address, size, swizzle).
// Formats 8888, DXT1, DXT2/3 and DXT4/5 are decoded (all the HUD and menus use in the census); others are drawn as
// their color alone and named once in the log. Tiled textures are untiled with the Xenos 2D tiling (as rexglue's and
// Xenia's texture code computes it). A texture 16 texels or less on a side with packed mips (fetch constant dword 5
// bit 11) keeps its top level 16 texels into the packed tile: down when it is wider than tall, else across.
//
// Step 4's first try at 3D: the generic surface nodes (kind 0x8201C3FC, Render 0x82143F70) drawn as meshes under the
// 2D, untextured (a color for each material) and lit by ngl_vk's fixed light, each scene with its own camera. What is
// read (runs acct2-bro-side6 and -side7):
//   - node +0x0C the object: its world matrix at +0x00 (Direct3D's 4x4, row-vector; its w column is left 0);
//   - node +0x10 the surface: +0x10 / +0x20 its box (smallest and largest corner, in its own space); at +0x40 and
//     +0x50 two buffers, each {Direct3D buffer, start, count, index size}. Which is which the buffer tells: a vertex
//     buffer's fetch constant (+0x18) has type 3 in its low two bits; an index buffer has its address there and its
//     size in bytes at +0x1C. +0x70 is taken as the vertex stride in bytes;
//   - the scene: +0x130 the projection, +0x170 the view, +0x1B0 their product (Direct3D's, row-vector). Only scenes
//     with a perspective camera are drawn. When the projection's depth runs backward (near 1, far 0), z is turned
//     over (w - z) for ngl_vk, whose depth runs near 0, far 1. +0x3D8..+0x3E4 and +0x3F0 its fog (FogWith), the way
//     Big Red One's nglSetFog keeps it (from 2026-11-05 13:50).
// A vertex's position is taken as three floats at its start; a surface whose first corners fall outside its box is
// left out, and every reason a surface is left out is counted in the log.
//
// cod3mp (side87) uses its own addresses for the same globals, vtables and functions (the decompile's tables, see
// kRootScene); the scenes' and nodes' offsets are the same NGL's in both programs.
//
// Direct3D's state at the draws (from 2026-11-05 13:00): after dumps 3 and 6 the next frames' draws read the device's
// shader constants (ngl_census.cpp's hooks on Direct3D's two draws call OnDraw); see kDevicePointer. The grass's
// draws save its shaders and vertex buffers (from 2026-11-05 15:40); see kGrassDrawReturn. The grass itself is drawn
// by ngl_vk's translation of the game's grass shaders (from 16:10); see GrassFor.

#include "game/ngl_side.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>  // rn42y
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>  // side212: the worker's thread name and CPUs
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32)  // [rexglue-native] 2026-11-09: the same on Windows (WorkerMain), and the program's path
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"  // side125: kActiveLocalClient (the depth of field's aim)
#include "game/guest.h"
#include "game/hd_textures.h"
#include "game/render_resolution.h"  // [rexglue-native] 2026-11-09: the native picture's size (ChooseTargetSize)
#include "game/screenshot.h"
#include "render/native_compare.h"  // [rexglue-native] 2026-11-09 rn33: the live A/B compare

#if defined(COD3_NGL_VK)
extern "C" {
#include "ngl_vk.h"
}
#endif
#if defined(COD3_NGL_VK)  // side120: the presenter, to show the native picture in the window
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/gpu_plugin.h>  // side124: the plugin's host backend (cod3_native_vulkan)
#include <rex/graphics/host_renderer.h>  // side215: the game's own draws, read from the GPU's registers
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>  // side138: physical_membase (GpuPointer)
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>
#endif

REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side, false, "cod3/Debug",
                    "Every 10 s for 80 s, draw the frame's 2D and its plain 3D surfaces with the native renderer "
                    "(ngl_vk) and save it beside the game's own screenshot (userdata/logs/screenshots/ngl-side-*); a "
                    "check for the native renderer")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side119: the first step off the emulated GPU: the native picture made every frame, not only every 10 s
REXCVAR_DEFINE_BOOL(cod3_ngl_side_live, false, "cod3/Debug",
                    "Draw every frame with the native renderer (ngl_vk) too, besides the emulated GPU's picture; "
                    "the log says how long it takes (pictures are still saved only with cod3_debug_ngl_side)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side124: off the emulated GPU (owner 2026-11-06 04:12: "We are using vulkan"): the plugin's host backend runs the
// game's GPU commands without drawing them, and ngl_vk draws every frame into the game's window (StartNativeVulkan)
REXCVAR_DEFINE_BOOL(cod3_native_vulkan, false, "cod3/Display",
                    "Draw the game with the native Vulkan renderer (in development): the Xbox 360 GPU emulation is "
                    "off, and every frame is drawn from the game's own scene lists")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// side210: ngl_vk on the window's own Vulkan device (the SDK presenter's), so each picture reaches the window on the
// GPU, through the game's gamma table in a shader, instead of being copied back to the CPU, widened there and copied
// up again (the GPU wait before the copy and the fill were about 3 ms of each 8 ms frame of the game's main thread)
REXCVAR_DEFINE_BOOL(cod3_native_gpu_geometry, true, "cod3/Display",
                    "With cod3_native_vulkan, keep the models' corners in the GPU's own memory (copied in as they "
                    "change) instead of memory the GPU reads over the bus every frame (side220)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(cod3_native_shared_device, false, "cod3/Display",
                    "With cod3_native_vulkan, draw on the window's own Vulkan device and hand each picture to the "
                    "window on the GPU, with no copy back to the CPU (in testing)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// side212: each native frame read from the game's lists and drawn on a thread of its own ("ngl worker") while the
// game's main thread goes on with the next frame. The game keeps two buffers of list memory and switches them in
// nglBeginFrame (0x82130808), so a frame's lists stay as they are until the frame after next begins, and its own render
// job reads them on another thread meanwhile. The worker is done before the next nglListSend goes on.
REXCVAR_DEFINE_BOOL(cod3_native_worker, false, "cod3/Display",
                    "With cod3_native_vulkan, read and draw each native frame on a thread of its own while the game "
                    "goes on with the next frame (in testing)");
// side222 [acct2-bro] 2026-11-07: in native mode the host backend draws nothing, so the game's own Direct3D draws
// (on its render job's threads: 2.2 ms a frame in St-Bert, and the GPU commands thread that reads them, 52% busy) are
// work with no picture. This leaves them out; everything else the render job does (state, fences, interrupts, the
// swap) still happens. Not with cod3_debug_host_draws, which counts those draws.
REXCVAR_DEFINE_BOOL(cod3_native_skip_game_draws, false, "cod3/Display",
                    "With cod3_native_vulkan, leave out the game's own Direct3D draws, which nothing shows in native "
                    "mode (in testing)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side223 [acct2-bro] 2026-11-07 (found by acct2-cod3-fx): the effects (smoke, fire, dust, sparks) are kept from the
// game's own particle flushes, which its render job makes after nglListSend, so EffectDraws draws them one list late
// with the camera they were flushed with: last frame's, over this frame's world. Turning, the smoke swims against the
// world by one frame of turn. This draws a flush made with last frame's world camera with this frame's instead (the
// particles' places stay a frame old; anything flushed with another camera keeps its own).
REXCVAR_DEFINE_BOOL(cod3_native_effects_current_camera, false, "cod3/Display",
                    "With cod3_native_vulkan, draw the effects with this frame's camera, not the one they were kept "
                    "with a frame ago (in testing)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-09 rn16: the game's rain (The Mace's storm), drawn natively by RainDraws
REXCVAR_DEFINE_BOOL(cod3_native_rain, true, "cod3/Display",
                    "With cod3_native_vulkan, draw the game's rain natively")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side224 [acct2-bro] 2026-11-07 (measured by acct2-cod3-fx: native St-Bert shows frames with no smoke at all, 1-3 in
// a row, while the game flushes its effects every frame). OnListSend counts a list as sent before the game's
// nglListSend waits for the last list's render job, so that job's late particle flushes are counted with the new
// list: the frame drawn then finds none (or only some) of the last list's effects, and the next finds them mixed in.
// With this on, nglListSend's hook waits for the render job first (the wait the game itself does next), so every
// flush is counted with the list it belongs to and each frame draws a whole list's effects.
REXCVAR_DEFINE_BOOL(cod3_native_effects_whole_list, false, "cod3/Display",
                    "With cod3_native_vulkan, wait for the game's render job before counting the next list sent, so "
                    "each frame's effects are a whole list's (in testing)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side228 [acct2-bro] 2026-11-08: the native caches are keyed by guest addresses and ngl_vk never freed anything, so a
// second level in one session could show the last level's textures and meshes where the new ones landed, and a few
// levels used up ngl_vk (2048 textures, 4M mesh corners). With this on: a texture or mesh whose description changed
// at the same address is made again, and everything is forgotten (ngl_vk_forget) before ngl_vk runs out.
REXCVAR_DEFINE_BOOL(cod3_native_forget_level, false, "cod3/Display",
                    "With cod3_native_vulkan, notice textures and meshes the game made again at the same place (a new "
                    "level) and start over before the native renderer runs out of room (in testing)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-09 (owner 11-08 13:31: "I'm seeing someblocks like stuff"; 11-09 09:35: "Native rendering
// should work"): the native picture was always drawn at 1280x720 and stretched to the window. It is now drawn at the
// size the game itself is drawn at for the window (the render plan: cod3_render_resolution, cod3_render_percent and the
// window's size), kept 16:9 like the native picture's 2D screen and camera. Chosen once, when ngl_vk starts
// (ChooseTargetSize); a resolution change while the game runs applies at the next start.
REXCVAR_DEFINE_STRING(cod3_native_resolution, "", "cod3/Display",
                      "With cod3_native_vulkan, the size the native picture is drawn at: empty = the size the game is "
                      "drawn at for the window (cod3_render_resolution), or 720p, 1080p, 1440p, 2160p or WxH (kept "
                      "16:9; 1280x720 was the old fixed size). Applies at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// [rexglue-native] 2026-11-09 (owner 11-09 09:35: "Native rendering should work"): rn1's long native frames came with
// bursts of new textures, about 10 ms each, because ngl_vk copied every new texture to the GPU and waited for it on its
// own. With this on (ngl_vk_set_batched_textures) a frame's new textures wait in a staging buffer and go to the GPU
// with the frame's own commands, with no wait. The picture is the same; off is the old way, for comparing.
REXCVAR_DEFINE_BOOL(cod3_native_batched_textures, true, "cod3/Display",
                    "With cod3_native_vulkan, send a frame's new textures to the graphics card together with the frame "
                    "instead of one at a time with a wait each (fewer long frames when much comes into view at once)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side215 [acct2-bro] 2026-11-07: the sky and the bright effects are about 1.4x brighter in the game's own picture
// than natively, a scale on every sky layer (NOTES 11-07 15:32). A color exponent bias on their render target
// (RB_COLOR_INFO bits 20-25: the shader's output x 2^bias) would do that and is in no NGL list. In native mode the
// host backend still walks the game's GPU commands, so this reads each of the game's draws' registers there.
REXCVAR_DEFINE_BOOL(cod3_debug_host_draws, false, "cod3/Debug",
                    "With cod3_native_vulkan: count the game's own draws by render target format, exponent bias and "
                    "blend (read from the GPU's registers) and by shader pair, logged every 10 s");
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_arms, 0, "cod3/Debug",
                     "With cod3_debug_ngl_side, how the first-person arms (the front scene's skinned surfaces) are drawn "
                     "in the native picture: 0 as the rest, 1 plain white and unlit, 2 their texture unlit, 3 plain and "
                     "lit (to see where they land and what hides them)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn41s: how many compare pictures a debug run takes, and how often (owner 08:12: "take
// pictures all throughout the run"): each is the native picture and the game's own of the same moment (Dump). The
// defaults are the ones before (every 600 frames, 8 of them).
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_shot_every, 600, "cod3/Debug",
                     "With cod3_debug_ngl_side, a native picture and the game's own of the same moment every this many "
                     "frames (cod3_debug_ngl_side_shots of them)")
    .range(60, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_shots, 8, "cod3/Debug",
                     "With cod3_debug_ngl_side, how many native pictures (each beside the game's own) a run takes")
    .range(1, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// rn42m: each compare moment taken on this many frames in a row (each counts as one of cod3_debug_ngl_side_shots)
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_shot_burst, 1, "cod3/Debug",
                     "With cod3_debug_ngl_side, take each compare picture on this many frames in a row")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn42j: pictures of live native frames in a row (owner 10:29-10:30: the grass and bushes
// "flicker with lighting", and smoke and dust, "mostly just the native renderer"). The compare pictures are drawn on
// the game's thread at the list send; these are the live frames as the worker draws and shows them, so what changes
// from one frame to the next can be seen. From frame cod3_debug_ngl_side_live_shot_at, this many in a row, again
// every cod3_debug_ngl_side_shot_every frames, three times (userdata/logs/screenshots/ngl-side-live-<frame>-vk.ppm).
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_live_shots, 0, "cod3/Debug",
                     "With cod3_native_vulkan and cod3_debug_ngl_side, save this many live native frames in a row as "
                     "shown (0: none), three times, cod3_debug_ngl_side_shot_every frames apart")
    .range(0, 30)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_live_shot_at, 1800, "cod3/Debug",
                     "The first live native frame cod3_debug_ngl_side_live_shots saves")
    .range(1, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_shadow, true, "cod3/Debug",
                    "With cod3_debug_ngl_side, the sun's shadow in the native picture: the soldiers, props and trees "
                    "cast it and their sun is dimmed by it, as the game's sun cascades do")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side127: a check of the native blur on its own, whatever the game asks for
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_blur, 0, "cod3/Debug",
                     "With cod3_native_vulkan, blur the 3D world behind the scenes drawn in front of it by this much "
                     "(percent; 0: only where the game blurs), to check the native blur")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side134: the meshes' own normals (as Big Red One's native side reads them, skinned ones turned by their bones), so
// the lit models (the arms, the gun, the soldiers) are shaded smoothly as the game's vertex shaders do
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_normals, true, "cod3/Debug",
                    "With cod3_native_vulkan, light the models by their vertices' own normals (off: by each "
                    "triangle's, as before); read when a mesh is first drawn")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// [acct2-bro] 2026-11-07 side165: the lightmapped world lit as the game's own pixel shaders do (their constant tables
// name everything; results/acct2-bro-tools/ps_names.py): the terrain's 141294CDC7C97127 and 478FCC79268D229F, the
// plain E72CD0D5E1523920 and B2367F8011B6042A and their bumped kin (6D703BD4597C9D00 ...) all light by
//   LightMap x (1 + 0.2 x the normal's up part) + COD_SHADOW_PROJCOLOR (the sun's color) x LightMap's alpha x N.L,
// the sun's way from COD_SHADOW_POSITION, then times the diffuse map (plus two nearby lights, LIGHT_COLOR1/2, and the
// specular maps, not drawn natively yet). So the lightmap's alpha is how much of the sun reaches there (side160-162's
// color x alpha was the wrong use of it), and the lightmap isn't doubled. (F38DD169E44123DB, which side161 took for the
// world's, is SpeedTree's fronds: samCompositeLeafMap, samSelfShadowMap, g_fDiffuseScale c16, g_vMaterialAmbient c17.)
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_world_light, true, "cod3/Debug",
                    "With cod3_native_vulkan, light the lightmapped world as the game's shaders do: the lightmap x "
                    "(1 + 0.2 x the normal's up part) plus the sun x the lightmap's alpha (off: the lightmap doubled, "
                    "as before 2026-11-07)");
// side167: whether the per-pixel lit models' normal and specular maps are used (to see what darkens Black Burn's
// tanks: side167 said not those, the same picture on and off)
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_bumped, true, "cod3/Debug",
                    "With cod3_native_vulkan, the per-pixel lit models (the gun, soldiers, vehicles) take their normal "
                    "and specular maps (off: their vertex normals only)");
// [rexglue-native] 2026-11-09 rn24: the uniforms as the game's fabric pixel shader (3BA30872B3955C23) draws them: its
// woven swatch folded in by the fabric mask, 0.51 x the diffuse with its sheen, and highlights from the sun and the
// nearest point light (ngl_vk.h fabric_mask). Read from the shader's own code (work folder rn21/SOLDIER-SHADERS.md);
// its light comes from the game's light block as before (0x82142360, uploaded unscaled by 0x82154DC0).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_fabric, true, "cod3/Debug",
                    "With cod3_native_vulkan, the uniforms take the game's fabric look (the woven swatch by the "
                    "fabric mask, the sheen, highlights from the sun and the nearest point light; the view model's "
                    "sleeves 0.51 x the diffuse, the soldiers' own the sun never under the shadow's grey level; the "
                    "sun's color and place as their pass binds them); off: drawn as other bumped models");
// [rexglue-native] 2026-11-09 rn32: the soldiers' heads as the game's skin pixel shader (3949D29E09A78AA1) draws them:
// 1.215 x the diffuse where the skin is seen edge-on, highlights of power 68.528 masked by 0.4 x the specular map mixed
// 0.38 toward the noise swatch (at 11.018 repeats), and the sun as the uniforms' (ngl_vk.h noise_swatch). Read from the
// shader's own code (work folder rn9/dump/shader_3949D29E09A78AA1.ucode.frag) and its literals (rn29).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_skin, true, "cod3/Debug",
                    "With cod3_native_vulkan, the soldiers' heads take the game's skin look (lighter where seen "
                    "edge-on, highlights masked by the noise swatch, the sun as their pass binds it); off: drawn as "
                    "other bumped models");
// [rexglue-native] 2026-11-09 rn34: the guns' metal as the game's pixel shader CFC45995D1294CCE draws it (the Springfield,
// the BREN): the normal map, Metal_Hue x the diffuse, the light x texture_brightness, highlights of SpecularPower and
// Reflection_Amt x the environment cube (read as a cube, ngl_vk_texture_cube), all times the specular map + the scratch
// map + the diffuse turned toward a blue-grey at the rim (ngl_vk.h metal_cube). Read from the shader's own code (work
// folder rn9/dump/shader_CFC45995D1294CCE.ucode.frag), its literals (rn20) and the material's values at its draws (rn22).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_metal, true, "cod3/Debug",
                    "With cod3_native_vulkan, the guns' metal takes the game's look (its environment cube, scratch map, "
                    "hue and rim, the material's own brightness, reflection and highlight power); off: drawn shiny with "
                    "a flat environment map, as before");
// [rexglue-native] 2026-11-09 rn36: the vehicles' detail-mapped metal as the game's pixel shaders C90DF51FF5AAF4F8 (in the
// sun's shadow range) and 8E7F713B9E3C1FAD (beyond it) draw it (The Mace's tank body): the Detail map folded into the
// normal at the material's repeat (15.2), highlights of its power (30) from the sun and the nearest point light, and
// the sun's color and place as the pass binds them, in the shadow range never under COD_SHADOW_GREYLEVEL and eased
// toward 1 by COD_SHADOW_FARZ x the view depth (ngl_vk.h detail_map). Read from the shaders' own code (work folder
// rn9/dump) and their constants at the game's draws (rn35b).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_detail, true, "cod3/Debug",
                    "With cod3_native_vulkan, the vehicles' detail-mapped metal (the tank's body) takes the game's look "
                    "(its detail normal map, highlights of its own power, the sun as its pass binds it, never under "
                    "the shadow's grey level); off: drawn as other bumped models")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn39: the bumped vehicle parts with a specular map as the game's pixel shaders
// B2E0DA0062DFF88D, 9A3D3175A6CAFE56 (treads) and 7D4E7CAD9503762D (wheels) draw them (The Mace's tank; rn39d: and
// their unshadowed passes beyond the sun's shadow range, BA59B7419D390DB8 / E470EE60FDF3D47D and the treads'): the
// sun's color and place as the pass binds them (B2E0DA00's in the shadow never under COD_SHADOW_GREYLEVEL and eased
// toward 1 by COD_SHADOW_FARZ x the view depth), the normal map's scale and the highlights' power as the material says
// (ngl_vk.h vehicle_bumped; ReadVehiclePass). Read from those shaders' microcode (work folder acct2-bro-shaders) and
// their constants at the game's draws (rn35b: B2E0DA00's c27 the sun's place, c28 its color 0.905 0.868 0.749, c29
// 0.001, c30 0.4, c31 1, c32 30).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_vehicle, true, "cod3/Debug",
                    "With cod3_native_vulkan, the vehicles' bumped parts (the tank's treads and wheels) take the "
                    "game's look (the sun as their pass binds it, never under the shadow's grey level where the game "
                    "holds it there, highlights of their own power); off: drawn as other bumped models")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn40t: the soldiers' helmets as the game's pixel shader 1D4725CEF0551EBB draws them
// (ReadHelmetPass); rn40x: and those with a woven swatch (and two soldiers' bodies) as ADC02DCE4FDC9253 does
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_helmets, true, "cod3/Debug",
                    "With cod3_native_vulkan, the soldiers' helmets take the game's look (the sun as their pass binds "
                    "it, never under the shadow's grey level, brighter edge-on, highlights of their own power, the "
                    "woven ones' swatch); off: drawn as other bumped models")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn40y: the soldiers' heads drawn without a specular map as the game's 2340DF59702A9AD4
// draws them (ReadPlainHeadPass)
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_plain_heads, true, "cod3/Debug",
                    "With cod3_native_vulkan, the soldiers' heads drawn without a specular map take the game's look "
                    "(the sun as their pass binds it, never under the shadow's grey level, fading out softly where "
                    "the surface turns from the light); off: drawn as other bumped models")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn40: glass with an environment cube (the soldiers' goggle lenses) as the game's pixel
// shader FC767C09DBD34C2A draws it (ngl_vk.h lens_cube; ReadLensPass): blended by its alpha, the cube and the highlights
// where the glass is clear. Before rn40 it was drawn opaque and "shiny", the cube read as a 2D sphere map: white.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_lens, true, "cod3/Debug",
                    "With cod3_native_vulkan, glass with an environment cube (the soldiers' goggle lenses) takes the "
                    "game's look (blended by its alpha, the cube's reflection and highlights where it is clear, the "
                    "sun as its pass binds it); off: drawn shiny as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn40e: the soldiers' eyes with a Reflection_Cube_Map (The Mace's Polish infantry) as
// the game's pixel shader 5931940F174EA055 draws them (ngl_vk.h lens_opaque; ReadLensPass's kLensEyes). Before rn40e
// they were drawn as generic bumped models without the cube. (rn40e-rn40m took these draws for the helmets; rn40n
// painted them magenta and rn40p measured them: a thin piece at the front of the face, 74 corners. The helmets are
// 1D4725CEF0551EBB's: cod3_debug_ngl_side_helmets.)
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_eyes, true, "cod3/Debug",
                    "With cod3_native_vulkan, the soldiers' eyes with a reflection cube take the game's look (the "
                    "cube's reflection and highlights of power 500, the sun shadowed as the game holds it); off: drawn "
                    "as other bumped models")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn40m: a check of the eyes' parts (and rn40r: of the helmets' place), to see which
// differs from the game's picture
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_eyes_show, 0, "cod3/Debug",
                     "With cod3_native_vulkan, the eyes drawn as the game's 5931940F174EA055: 0 as the game draws "
                     "them; 1 only their light (as if their color map were white, no highlights or reflection); 2 only "
                     "their color map (lit by 1); 3 magenta (where they are drawn); 4 that, and the watched maps' "
                     "generic bumped draws with a specular map flat white")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn40j: the light the game handed a model's draws is kept by its object, but some objects
// are made anew each frame at another address (rn40i: the Polish infantry's helmet at one place was A9710140,
// A8B10100, A8B0FE40, A970FE80 ... on frames 2900-2904, so the native draw often found no light kept for it, or one
// 36-56 frames old, and drew it with no light rows: near black). With this on, a model with no recent light of
// its own takes the light the game handed lately to a model drawn where it stands (same light set first, else the
// nearest within kKeptLightNear).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_light_by_place, true, "cod3/Debug",
                    "With cod3_native_vulkan, a model whose object has no recent light from the game's draws (objects "
                    "made anew each frame) takes the light the game handed lately to a model where it stands; off: "
                    "only its own object's light (else the light set's)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn42v: the owner's "burnt slab" (09:03: the underside of our vehicle, with the NPCs' poses
// wrong in it). The scene SolidDraws leaves out as side83's third (drawn first, order 0, no color target of its own:
// +0x328 is 0, the game refills no model light for it) has a near plane of 0.23 to 4.6 units. Below kFrontNear x the
// world's (rn42q's census: 0.23-0.26 at St-Bert 1800-1951) it was taken as the gun's scene and drawn in front of
// everything, so it came and went (2100: near 4.6, left out). The gun's own scene draws into the world's target
// (8291100C at St-Bert, order 6, near 0.05). With this on, a scene is drawn in front of the world only if it draws
// into the world's target too.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_front_target, true, "cod3/Debug",
                    "With cod3_native_vulkan, a scene is drawn in front of the world (as the gun's) only if it draws "
                    "into the world's own target; off: by its near plane alone (a scene without a target of its own "
                    "can then cover the picture)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn42w: the lit models' lighting flicker (owner 10:29: "the grass/bushes flicker with
// lighting. There are a lot of other effects that also do this"). rn42u's live frames: a lit model's sun was its
// node's state sun, else the last node's (g_sun) or its light set's, so it changed with the order of the draws and the
// object read (Saint-Lo: 0.960, 1.148, 0.960 on frames in a row; The Mace: 0.773 to 0.373; the forest: 0.353 to
// 1.097). With this on, a lit model lit by the light the game handed it also takes that light's main light as its
// sun: the same object's light block (+0x00 its color, COD_SHADOW_PROJCOLOR; +0x10 its way), as the game's shaders
// get it. The draws whose pass binds their sun (uniforms, skin, helmets, metal, vehicles) and the front scene keep theirs.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_model_sun, true, "cod3/Debug",
                    "With cod3_native_vulkan, a lit model takes the sun the game handed its draws (its light block's "
                    "main light, kept with its irradiance rows); off: its node's sun, else the last node's or its light "
                    "set's (which changed from frame to frame)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn42z: a check, off: the draws whose pass binds their sun (the uniforms, skin, helmets,
// gun and detail metal, vehicles) take their light block's main light's color too. rn42z: their pass-bound sun, read
// from the state's value block after the game has written it again for other passes, read 0.773, 0.764 or 0.373 for
// some draws on some frames (The Mace's tank group) where their block's main light stayed 0.905 (the GPU's own
// COD_SHADOW_PROJCOLOR there, rn22); but at St-Bert 2101-2106 soldiers in shade have a block main light of 0.509,
// 0.103 or 0 where their pass binds the scene's 0.614 (those shaders take the scene's sun and shade it by the shadow
// map), so the block's value would darken them: off until the pass's own value is read when the game draws it.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_model_sun_pass, false, "cod3/Debug",
                    "A check, with cod3_debug_ngl_side_model_sun: the draws whose pass binds their sun take the color "
                    "of their light block's main light too (off: the pass-bound color)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn43a: the values a pass binds from its shader state's value block (the sun's color and
// place, the shadow's fade and floor, a material's numbers), as they were when the game drew the object. The pass's
// parameter fill (0x82142F20: shader state, object, surface) writes that block for each object it draws, and states
// are shared, so read later it holds the last object's: The Mace's tank read the sun of 0.773 another draw had (the
// lens notes' "0.905 ... and 0.773 ... for two soldiers"), on some frames only (rn42z). With this on, each lit draw
// keeps its object's values of the block at the offsets the native side reads (OnPass gives the state, KeepModelLight
// keeps them with the light), and the native draw of that object (or of the model its light is taken from, rn40j)
// reads them; off, or when none were kept, the block as it is now (as before).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_drawn_values, true, "cod3/Debug",
                    "With cod3_native_vulkan, the values a model's pass binds from its shader state (its sun's color "
                    "and place, its shadow's fade and floor) are those the game drew the model with; off: read when "
                    "the native frame is built (the last object's, which changed from frame to frame)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn43c: objects are made anew each frame and their addresses come round again every few
// frames, so the light kept at a model's address may be another model's (rn43a's The Mace: a soldier's took 0.905 for
// one frame where his own was 0.832). With this on, a model takes the light kept at its address only when it was
// kept where the model stands now (within kKeptLightNear, both by the object's +0x30), else one kept by place.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_own_light_place, true, "cod3/Debug",
                    "With cod3_native_vulkan, a model takes the light kept at its own address only when it was kept "
                    "where the model stands now (else another model's that had the address); off: any kept there "
                    "lately")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-09 rn35: the tank (rn34's live split: its body darker natively than the game's). A check:
// the draws whose color map's own name holds this text (case ignored; The Mace's tank is "sherman_firefly_c", its
// treads and wheels "sherman_tread_c", "sherman_wheel1_c") are watched from frame 2900. The native side logs each color
// map's draw once (its material's maps, how the native side draws it, the light it gives it, its pass's binding); the
// host-draw log (cod3_debug_host_draws) logs which pixel shaders the game draws the same texture with, and their
// constants. Debug only (cod3_debug_ngl_side).
REXCVAR_DEFINE_STRING(cod3_debug_ngl_side_watch, "", "cod3/Debug",
                      "With cod3_debug_ngl_side: log the native and the game's draws of the color maps whose own name "
                      "holds this text, from frame 2900 (empty: none; a check)");
// side168: how a model with its own light set is lit. The game's per-pixel model shaders (7D4E7CAD9503762D and kin:
// IrradianceCoeffs_0..8 c18-c26, __1_COD_SHADOW_PROJCOLOR_cmn c28, position_Light0 c27, LIGHT_COLOR1 c17) light by
// the nine irradiance rows (the set's ambient and its other lights folded in) + the scene's sun (COD_SHADOW_PROJCOLOR
// x the shadow x N.L) + the nearest point light; side167's log: a soldier's set holds the sun at about half its color
// (Saint-Lo 0.595 against 1.148), a tank's a fixed light (0.55 from 45 degrees above, ambient 0.32), and natively those
// replaced the sun. 0: the set's lights alone, held to 1 (as before); 1: the scene's sun first (shadowed), then the
// set's own lights, not held to 1; 2: the scene's sun in place of the set's first light, not held. side168: 1 made
// Saint-Lo as bright as the game but further off (diff 16.2 -> 19.3: too bright where the sun meets them); 2 the closest
// (16.2 -> 14.8 at frame 1800; Black Burn 46 -> 52 there), so 2 is the default.
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_model_light, 2, "cod3/Debug",
                     "With cod3_native_vulkan, how models with their own light set are lit: 0 their set's lights (held "
                     "to 1), 1 the scene's sun plus their set's lights, 2 the scene's sun in place of their set's first")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Half of those shaders (the ones with __COD_SHADOW_DEPTHMAP_cmn_Sampler: 26B1595E0EFD8234, 478FCC79268D229F ...) also
// take the sun's shadow map in the sun's part, so the soldiers' and props' shadows fall on the ground.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_world_shadow, true, "cod3/Debug",
                    "With cod3_debug_ngl_side_world_light and the sun's shadow, the lightmapped world takes the "
                    "shadows of the soldiers and props in its sun (off: only the lightmap's own)");
// side170: the game's "apsColorUVANode" render nodes (campaign vtable 0x820152E0, Render 0x8210FD10 -> 0x82110278; made
// every frame by 0x820FCD70 for a mesh drawn with a color and moving texture coordinates; three in Black Burn, where
// the middle of the native picture is about 40% darker than the game's). Not drawn natively yet. A check: this leaves
// them out of the game's own picture too, to see what they are; the log lists what they hold.
// side171: which of the sky's layers the native side draws, one bit each in the order the game draws them (-1: all), to
// see what each adds against the game's picture (Black Burn's far middle is near its fog color in the game).
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_sky_layers, -1, "cod3/Debug",
                     "The sky layers the native side draws, one bit each in the game's order (-1: all; a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side174: the sky's first, opaque layer drawn through the blended pipeline instead (its picture and corners are opaque,
// so the result should be the same): Black Burn's opaque 16 x 16 base decodes to a light blue (side173) yet adds nothing.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_sky_base_blended, false, "cod3/Debug",
                    "Draw the sky's first, opaque layer through the blended pipeline (a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side174: a sky layer's drift through this frame's own corners (ngl_vk_frame_corners), or left still on the mesh's
// corners: Black Burn's base layer is the one layer that drifts, and the one drawn black.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_sky_drift, true, "cod3/Debug",
                    "Move the sky layers' textures by their drift (off: they stay still; a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side175: neither changed it (side174), so: the layers drawn in a flat magenta instead of their texture, one bit each
// in the game's order, to see where their triangles land.
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_sky_flat, 0, "cod3/Debug",
                     "The sky layers drawn in a flat magenta instead of their texture, one bit each in the game's order "
                     "(a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side176: the color the native 3D starts from. Black Burn's sky layers (side171-175) cover a far middle the game
// shows near its fog color; the game clears each scene first (its clear color at scene +0x3C0), ngl_vk to black.
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_clear, 0, "cod3/Debug",
                     "The color the native 3D starts from: 0 black, 1 the world's scene's clear color, 2 its fog color "
                     "(a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side179: two node kinds the native side doesn't draw (Black Burn: 28 and 10 of them), left out of the game's own
// picture to see what they bring to its far middle
REXCVAR_DEFINE_INT32(cod3_debug_skip_game_draws, 0, "cod3/Debug",
                     "Node kinds left out of the game's own picture, one bit each: 1 the kind 0x82001D2C (Render "
                     "0x820D4F70), 2 the kind 0x82003454 (Render 0x820E0AB8) (campaign; a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-09 rn11: one effect flush drawn natively in a flat magenta, picked by its texture's address
// (as the "effect shaders" log lines give it), to see where its quads land (The Mace's rain, rn10b's squares)
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_effect_mark, 0, "cod3/Debug",
                     "The effect flushes whose texture is at this address are drawn natively in a flat magenta (0: "
                     "none; a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// rn12: the same flush left out of the game's own picture, to see what it brings there (rn11: 0x10444000, a 16x16
// DXT1 framed square, is drawn natively as white-edged squares the game's picture never shows)
REXCVAR_DEFINE_INT32(cod3_debug_skip_game_effect, 0, "cod3/Debug",
                     "The effect flushes whose texture is at this address are left out of the game's own picture (0: "
                     "none; a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// rn13: the game's draws from one call site (the return address of Direct3D's draw, as the "draws by call site" lines
// list them) left out of the game's own picture, to find what draws The Mace's rain. The address is given as a
// signed 32-bit number (the cvar's type): 0x820FAF80 is 0x820FAF80 - 0x100000000 = -2112901248.
REXCVAR_DEFINE_INT32(cod3_debug_skip_game_call_site, 0, "cod3/Debug",
                     "The game's draws from this call site (its address as a signed 32-bit number) are left out of its "
                     "own picture (0: none; a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side191: the sky's layers left out of the game's own picture, one bit each in the order the game draws them in a
// list (the order cod3_debug_ngl_side_sky_layers counts natively), to compare the two pictures one layer at a time
REXCVAR_DEFINE_INT32(cod3_debug_skip_game_sky_layers, 0, "cod3/Debug",
                     "The sky layers left out of the game's own picture, one bit each in the game's order (a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side192: where a texture's smaller levels come from. side191: with the sky's opaque base layer alone in both
// pictures, Island's is 1.4x brighter in the game, and Black Burn's (a 16 x 16 light blue) comes out nearly black
// natively, as if the levels the GPU samples at a distance were decoded wrong. 0: the game's own levels
// (DecodeLevelsFetch, as before); 1: each level made from the one above, 2 x 2 averaged (BoxLevels); 2: the top level
// alone. Read when a texture is made (a check).
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_texture_levels, 0, "cod3/Debug",
                     "Where the native textures' smaller levels come from: 0 the game's own, 1 made from the top "
                     "level, 2 none (a check)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side183: the lightmapped world's two nearby lights (its shaders' LIGHT_POSITION1/2, LIGHT_COLOR1/2, LIGHT_INVFAR1/2),
// from each object's light set (side182)
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_world_nearby_lights, true, "cod3/Debug",
                    "With cod3_native_vulkan, light the lightmapped world by its two nearby point lights too, as the "
                    "game's shaders do")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side195: the terrain's second layer (pixel shader 478FCC79268D229F and kin: DiffuseMapB at its own coordinates,
// mixed by a weight, diffuse = A x (1 - w) + B x w, where the vertex shader 0FE885A9ED8C96A7 takes w from the corners'
// color's alpha and B's coordinates from their third texture coordinates). Natively drawn again over the first layer,
// blended by w and lit the same way (the light and the fog are the same for both, so the sum is the game's mix).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_world_second_layer, true, "cod3/Debug",
                    "With cod3_native_vulkan, mix the lightmapped world's second layer (DiffuseMapB) in by its "
                    "corners' weight, as the game's terrain shader does")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side200: a check for the sky's factor: the game's own post effects left out of its picture (the native side still
// reads their parameters), one bit a type (1 << type: 2 the tint, 8 the color grading, ...).
REXCVAR_DEFINE_INT32(cod3_debug_skip_game_post_effects, 0, "cod3/Debug",
                     "A check: leave these post effect types out of the game's own picture, one bit each (1 << type: "
                     "2 the tint, 4 the brightness, 8 the color grading, 16 the motion blur); 0 none")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side202: post effect 3 drawn natively (ColorLookup): a color look-up through the 64 x 64 table the game puts on
// texture stage 1. Campaign only for now (its addresses).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_color_lookup, true, "cod3/Debug",
                    "Draw the game's post effect 3 (a color look-up through a 64 x 64 table) natively (side204: on; "
                    "with the table read a page on, Island's table is the identity with a slight blue curve and the "
                    "picture is as close to the game's as without it; side202's green came from the table read a page "
                    "early)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_lookup_pages, 1, "cod3/Debug",
                     "Read post effect 3's table this many 4 KB pages after its fetch constant's address when that "
                     "address is in the 0xE0000000 view (side204: the view sits 4 KB into physical memory, and read at "
                     "the address itself the table's rows came out turned by 16, one page; 0: as before)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn41b: post effect 7 drawn natively (Film, ngl_vk_set_film): the blur, grain and gray of a
// concussion, which the game starts at an explosion nearby and fades over seconds. In both games (cod3mp's dispatcher
// has the same branch; kPostEffect7Time).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_film, true, "cod3/Debug",
                    "With cod3_native_vulkan, draw the game's post effect 7 natively: the blur, grain and gray of a "
                    "concussion after an explosion nearby; off: left out")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn41q: the game's unlit effects, by the effect a node's pass was compiled from
// (PassEffect): the lanterns' glass (standard_7_1: both its passes 5DD7AA099EE15C4F, the color map x the corners'
// colors with the map's alpha, blended, no light, no fog) and their flames (standard_10: 1DF1E9465C1429B4, the same
// from the map's 8-frame strip at 24 frames a second, by the scene's time). Natively they were lit as plain surfaces
// (dim, and the flames' whole strip at once).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_unlit_effects, true, "cod3/Debug",
                    "With cod3_native_vulkan, draw the game's unlit effects as it does (the lanterns' glass, "
                    "standard_7_1, and their animated flames, standard_10); off: lit as plain surfaces")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn41x: the SpeedTree leaves lit as the game lights them (sub_823F5028's effect: vertex
// shader 51732ACF1B6A87C5, pixel shader 6189945E43D7FBC8). Per leaf: light = ambient (c17) + max(N . toward the sun
// (c60), 0) x the sun's color (c58) x the leaf's dimming (its vertex's float at +76, 0 to 1), N its normal (the float3
// at +96, turned with the tree); out = saturate(the leaf map x light), cut out where the map's alpha is under the
// level's fade (c18); no sun shadow and no other light (c59, c61 are 0 on St-Bert). Natively they were the map times
// a fixed dark green (0x3C6432), so the sunlit crowns came out dark. Read once, as the leaf meshes are made once.
// rn41y: the ambient, the sun's color and its way are the ones the game hands the leaf effect (ReadLeafLight).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_leaf_light, true, "cod3/Debug",
                    "With cod3_native_vulkan, light the trees' leaves as the game does (the scene's ambient plus the "
                    "sun by each leaf's normal and dimming, no shadow); off: a fixed dark green, as before")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// [rexglue-native] 2026-11-10 rn42g: the effect flushes the game draws with only the frame's alpha written (RB_COLOR_MASK
// 0x8). The tank barrel's smoke (owner 09:13: "square") is two such effects with the 16 x 16 framed texture 0D84C000:
// the GPU draws the same quads with the same texture, but into the full-size frame's alpha alone (its other effects go
// to a quarter-size target with all four channels), so they never show in the game's colors. Natively they were drawn
// in color as white-framed squares. The mask is the device's copy of register 0x2104 (+0x285C).
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_effect_mask, true, "cod3/Debug",
                    "With cod3_native_vulkan, leave out the effect quads the game draws with no color channel "
                    "written (only the frame's alpha; the tank smoke's white-framed squares); off: drawn in color")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native] 2026-11-10 rn42j: how the soft effects (smoke, dust) fade where the scene is just behind them. The
// game's soft vertex shader (AACFBC52742E4628) hands the pixel shader (8356D96B00DE9DED) the corner's own depth z / w
// (its o0.z) beside the fade's scale (o0.w), and the pixel shader fades by saturate((the scene's depth - that) x the
// scale): the quad's depth is blended from its corners, not the pixel's own. Across a quad that isn't square to the
// view the two differ (the blend leans toward the near corners), and natively such smoke and dust lying along the
// ground faded out where the game's stays (St-Bert's street haze). 2: the corners' blend, as the game; 1: each
// pixel's depth (as before rn42j); 0: not faded.
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_effect_soft, 2, "cod3/Debug",
                     "With cod3_native_vulkan, how smoke and dust fade near what's behind them: 2 by the depth blended "
                     "from each quad's corners, as the game's shaders do; 1 by each pixel's own depth; 0 not at all")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// rn42m: an A/B of the above in one run (rn42j's frame 1800 came out hazier near the barn than the game's, and runs
// don't repeat): with it on, odd frames fade by the pixel's depth (1), even frames by the corners' (2); with
// cod3_debug_ngl_side_shot_burst 2, each compare picture's next frame is taken too, so the two sit a frame apart.
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_effect_soft_ab, false, "cod3/Debug",
                    "A check: odd frames fade the soft effects by the pixel's depth, even frames by the corners' "
                    "(cod3_debug_ngl_side_effect_soft 1 and 2)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// side184: the per-pixel lit models (the gun and arms, soldiers, vehicles: pixel shaders CDBAC6026B525050,
// 7D4E7CAD9503762D and kin) light by IrradianceCoeffs_0..8, nine rows the game's lighting (0x82142360) fills for each
// of their draws from the object's light set (its ambient and other lights folded in), plus the sun and the nearest
// point light. 0: off (the set's ambient and lights, as before); 1: every lit model the game handed rows; 2: only the
// bumped and shiny ones (the vertex-lit models' shaders, C4130C72D4721102 and kin, don't read the rows). side194:
// cod3mp too (its own addresses: kModelDrawReturn, kModelLights, kModelLightRows).
// side189: the puddles and other water on the lightmapped world (materials with no color map but an environment map
// and a normal map; island's "island_env" and "water_ripple01_n"). The game draws them with their own shader
// (99861CE203379D5B: two scrolling ripple normal maps, the environment by a Fresnel term (eta), Diffuse_Color,
// Opacity, the depth buffer to soften the edges) blended over the ground (SRC_ALPHA / INV_SRC_ALPHA); natively the
// material's first texture became an opaque color (bright cyan patches). 0: left out until that shader is done
// natively (the ground shows); 1: drawn as before.
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_water, 0, "cod3/Debug",
                     "With cod3_native_vulkan, the lightmapped world's water and puddles: 0 left out (the ground under "
                     "them shows), 1 drawn as before (opaque, their first texture as the color)")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_model_irradiance, 1, "cod3/Debug",
                     "With cod3_native_vulkan, light the per-pixel lit models by the light the game hands their draws "
                     "(its nine irradiance rows and nearest point light): 0 off, 1 every lit model, 2 bumped and "
                     "shiny ones only")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_skip_color_uv_nodes, false, "cod3/Debug",
                    "Leave the game's apsColorUVANode draws (campaign Render 0x8210FD10) out of its own picture, to see "
                    "what they are (a check)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#if defined(_WIN32)
REXCVAR_DECLARE(std::string, cod3_graphics_api);  // [rexglue-native] 2026-11-09: ui/hud_overlay.cpp (StartNativeVulkan)
#endif

namespace cod3::ngl_side {

// side230 [acct2-bro] 2026-11-08: how many levels have finished loading (OnLevelLoaded, on the game's main thread).
// LiveFrame compares it with the count it saw last, on its own thread (ForgetIfNewLevel).
static std::atomic<uint32_t> g_native_level_loads{0};
void OnLevelLoaded() { g_native_level_loads.fetch_add(1, std::memory_order_relaxed); }

namespace {

// side123: movies with the GPU emulation off. The game's movie player (XMEDIA's XMV player, campaign only; see
// docs/COD3_LIBRARIES.md, "The movie player and its decoders") decodes on threads of its own and draws and presents its
// frames through Direct3D itself, so nothing of a movie is in NGL's lists. Its WMV plug-in's GetOutput (0x8220D4E8,
// dev/x360-family/libs/codec_NOTES.md; the decompile's FUN_8220d4e8) has the decoder write each frame, planar 4:2:0,
// into the buffer it is given (r4): Y, width x height bytes, then U and V, width/2 x height/2 each. The decoder's
// state, *(r3 + 0x20), holds the width (+0x20) and the height (+0x24); when a frame came out its size is written to
// *r6 (0: none). The hook below keeps the latest frame as BGRX (BT.601, studio range, as the player's own shader
// converts it: 1.164, 1.596, 0.813, ...), and the native side draws it over the whole screen, under its quads, while
// frames keep coming (kMovieShownFor after the last one).
struct MovieFrames {
  std::mutex mutex;
  std::vector<uint8_t> frame;  // BGRX, the latest frame
  uint32_t width = 0, height = 0;
  uint64_t sequence = 0;
  std::chrono::steady_clock::time_point when{};
};
MovieFrames g_movie;
constexpr auto kMovieShownFor = std::chrono::milliseconds(250);

// One decoded frame (planar 4:2:0 at yuv) kept as BGRX; on the player's decoding thread.
[[maybe_unused]] void KeepMovieFrame(const uint8_t* yuv, uint32_t width, uint32_t height) {
  static thread_local std::vector<uint8_t> work;
  work.resize(size_t{width} * height * 4);
  const uint8_t* plane_y = yuv;
  const uint8_t* plane_u = plane_y + size_t{width} * height;
  const uint8_t* plane_v = plane_u + size_t{width / 2} * (height / 2);
  const auto clamp = [](int value) { return static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value); };
  for (uint32_t row = 0; row < height; ++row) {
    const uint8_t* y = plane_y + size_t{row} * width;
    const uint8_t* u = plane_u + size_t{row / 2} * (width / 2);
    const uint8_t* v = plane_v + size_t{row / 2} * (width / 2);
    uint8_t* out = work.data() + size_t{row} * width * 4;
    for (uint32_t column = 0; column < width; ++column, out += 4) {
      const int c = (y[column] - 16) * 298, d = u[column / 2] - 128, e = v[column / 2] - 128;
      out[0] = clamp((c + 516 * d + 128) >> 8);            // blue
      out[1] = clamp((c - 100 * d - 208 * e + 128) >> 8);  // green
      out[2] = clamp((c + 409 * e + 128) >> 8);            // red
      out[3] = 255;
    }
  }
  std::lock_guard<std::mutex> lock(g_movie.mutex);
  g_movie.frame.swap(work);
  g_movie.width = width;
  g_movie.height = height;
  ++g_movie.sequence;
  g_movie.when = std::chrono::steady_clock::now();
}

// The latest movie frame while a movie plays (copied into picture when it changed), or false.
[[maybe_unused]] bool MovieFrameNow(std::vector<uint8_t>& picture, uint32_t& width, uint32_t& height) {
  static uint64_t taken = 0;
  std::lock_guard<std::mutex> lock(g_movie.mutex);
  if (!g_movie.sequence || std::chrono::steady_clock::now() - g_movie.when > kMovieShownFor) return false;
  if (taken != g_movie.sequence || picture.size() != g_movie.frame.size()) {
    picture = g_movie.frame;
    taken = g_movie.sequence;
  }
  width = g_movie.width;
  height = g_movie.height;
  return true;
}

namespace g = cod3::guest;

// cod3mp's twins of the campaign's addresses below are from dev/waw-decomp/cod3/mp/globals_sp_to_mp.tsv (the votes
// in brackets) and cod3_relatives.tsv (side87); nothing else differs between the two programs here.
#if defined(COD3_MP)
constexpr uint32_t kRootScene = 0x82A8C130;       // (5 of 5)
constexpr uint32_t kListEndPointer = 0x825B2570;  // u32: the lists' end marker (12 of 12)
constexpr uint32_t kCurrentScene = 0x82A8C134;    // NGL's scene being drawn (133 of 133)
constexpr uint32_t kImageEnd = 0x82C00000;        // past the globals Readable follows (cod3mp's run to 0x82DD6000)
#else
constexpr uint32_t kRootScene = 0x8290E9A8;
constexpr uint32_t kListEndPointer = 0x825A1B34;  // u32: the lists' end marker
constexpr uint32_t kCurrentScene = 0x8290E9AC;    // NGL's scene being drawn
constexpr uint32_t kImageEnd = 0x82A00000;
#endif
// The blocks a compiled shader pass's constant lists index (bases 3, 4 and 6; base 7 through a pointer), and the
// tree parts' vertex formats (ShowTree).
#if defined(COD3_MP)
constexpr uint32_t kPassBase3 = 0x82A8E7F0, kPassBase4 = 0x82A8E1B0, kPassBase6 = 0x82A8F3F0;  // (14, 6, 5 of all)
constexpr uint32_t kPassBase7Pointer = 0x82A8E0C0;                                                // (6 of 6)
constexpr uint32_t kBranchFormat = 0x82BAD550, kFrondFormat = 0x82BAD544, kLeafFormat = 0x82BAD538;
#else
constexpr uint32_t kPassBase3 = 0x829118E0, kPassBase4 = 0x829112A0, kPassBase6 = 0x829124E0;
constexpr uint32_t kPassBase7Pointer = 0x829111B0;
constexpr uint32_t kBranchFormat = 0x82A1B8F4, kFrondFormat = 0x82A1B8E8, kLeafFormat = 0x82A1B8DC;
#endif
constexpr uint32_t kSceneNextSibling = 0x2F4;
constexpr uint32_t kSceneFirstChild = 0x2F8;
constexpr uint32_t kSceneColorTarget = 0x328;
constexpr uint32_t kSceneOpaque = 0x334;
constexpr uint32_t kSceneBlended = 0x338;
constexpr uint32_t kSceneLate = 0x344;  // a third list (count +0x348): the grass (GrassRenderNode, 0x823F2DB0)
// side176: how a scene clears its target before it is drawn: +0x3B8 the clear's flags, +0x3BC its depth, +0x3C0
// (16-byte aligned) its color, red, green, blue and alpha from 0 to 1 (the 360's D3D packs a float color for the
// target's format, D3D_PackClearColor 0x822EFAE8)
constexpr uint32_t kSceneClearFlags = 0x3B8;
constexpr uint32_t kSceneClearDepth = 0x3BC;
constexpr uint32_t kSceneClearColor = 0x3C0;
constexpr uint32_t kTargetFlags = 0x18;
constexpr uint32_t kTargetVirtualScreen = 0x2000;
constexpr uint32_t kNodeNext = 0x04;
constexpr uint32_t kNodeKey = 0x08;
constexpr uint32_t kNodePayload = 0x10;
#if defined(COD3_MP)
constexpr uint32_t kQuadNode = 0x820193E0;     // (1 of 1, each of these vtables)
constexpr uint32_t kStringNode = 0x82019DF0;
constexpr uint32_t kGenericNode = 0x8201D75C;
constexpr uint32_t kTreeNode = 0x8207A7A0;
constexpr uint32_t kGrassNode = 0x8207A700;
#else
constexpr uint32_t kQuadNode = 0x82018020;
constexpr uint32_t kStringNode = 0x82018A20;
constexpr uint32_t kGenericNode = 0x8201C3FC;  // the generic surface node (Render 0x82143F70)
constexpr uint32_t kTreeNode = 0x8207B838;     // SpeedTree and grass (Render 0x820C84F0)
constexpr uint32_t kGrassNode = 0x8207B798;    // GrassRenderNode (Render 0x823F79B0; see kGrassDrawReturn)
#endif
// side90: the old material classes' render nodes. For a mesh whose material is one of the old classes (cdSimple,
// cdWorld, ...: ngl_census.cpp's kDraws), nglListAddMesh makes a render node whose Render (its vtable's third entry)
// is that class's draw, laid out as the generic node is: +0x0C the object (its matrix first), +0x10 the surface, +0x14
// the material. side90mp found them on cod3mp's mp_merv (cdSimple's x7, cdWorld's x2: the crates' planks); the forest
// has none. Their materials have no sampler list: the color map is the first texture the material holds, and a
// cdWorld's lightmap the second.
#if defined(COD3_MP)
constexpr uint32_t kSimpleDraw = 0x820F2B50, kWorldDraw = 0x820F80B0;
#else
constexpr uint32_t kSimpleDraw = 0x820E0AB8, kWorldDraw = 0x820E6070;
#endif
constexpr uint32_t kVtableRender = 0x08;
enum OldClass { kNotOld, kOldSimple, kOldWorld };
constexpr uint32_t kQuadVertexBytes = 0x14;
constexpr uint32_t kQuadTexture = 0x60;
constexpr uint32_t kStringText = 0x0C;
constexpr uint32_t kStringFont = 0x10;
constexpr uint32_t kStringX = 0x14;
constexpr uint32_t kStringY = 0x18;
constexpr uint32_t kStringScaleX = 0x20;
constexpr uint32_t kStringScaleY = 0x24;
constexpr uint32_t kStringColor = 0x28;
// CoD3's font as the game holds it (run acct2-bro-side3; the on-disc FONT asset is in acct2-ocr's FONTS.md): +0x00
// its name, +0x04 the atlas (an NGL texture), +0x08 the glyph records, +0x10 / +0x14 its quads' blend (6, 0x07C00000),
// +0x18 0x102, +0x1C the line height, +0x20 the ascent, +0x24 the first code (0x20), +0x28 the glyph count (224),
// +0x2C the UV table (atlas pixels), then the records themselves at +0x38.
constexpr uint32_t kFontTexture = 0x04;
constexpr uint32_t kFontGlyphs = 0x08;
constexpr uint32_t kFontCellHeight = 0x1C;
constexpr uint32_t kFontFirst = 0x24;
constexpr uint32_t kFontCount = 0x28;
constexpr uint32_t kGlyphBytes = 0x1C;
constexpr uint32_t kTextureWidth = 0x00;
constexpr uint32_t kTextureHeight = 0x04;
constexpr uint32_t kTextureName = 0x10;  // a string object, its characters at +4 (0x8213F8A0)
constexpr uint32_t kTextureDirect3D = 0x14;
constexpr uint32_t kTextureFlags = 0x18;
constexpr uint32_t kTextureAnimated = 4;
constexpr uint32_t kTextureFrames = 0x28;
constexpr uint32_t kFetchConstant = 0x1C;
constexpr float kScreenWidth = 640.0f;
constexpr float kScreenHeight = 360.0f;  // the 640x480 screen squeezed to 16:9, so it fills the picture as in the game
constexpr float kScreenSqueeze = kScreenHeight / 480.0f;
// [rexglue-native] 2026-11-09: the native picture's size, chosen once when ngl_vk starts (ChooseTargetSize, before any
// frame is drawn; read only after that). Was a fixed 1280x720, which stays the size when nothing better is known.
uint32_t g_target_width = 1280;
uint32_t g_target_height = 720;
constexpr uint64_t kDumpEvery = 600;  // frames (rn41s: the live timing line's; the dumps' is DumpEvery)
// rn41s: the dumps' spacing and count (cod3_debug_ngl_side_shot_every, _shots)
uint64_t DumpEvery() { return static_cast<uint64_t>(std::max(60, REXCVAR_GET(cod3_debug_ngl_side_shot_every))); }
int DumpCount() { return std::clamp(REXCVAR_GET(cod3_debug_ngl_side_shots), 1, 200); }
uint64_t DumpBurst() { return static_cast<uint64_t>(std::clamp(REXCVAR_GET(cod3_debug_ngl_side_shot_burst), 1, 4)); }
// rn42j: whether a live frame is one cod3_debug_ngl_side_live_shots saves: from cod3_debug_ngl_side_live_shot_at, that
// many in a row, again every DumpEvery() frames, three times
bool LiveShotFrame(uint64_t frame) {
  const int32_t count = REXCVAR_GET(cod3_debug_ngl_side_live_shots);
  const uint64_t at = static_cast<uint64_t>(std::max(1, REXCVAR_GET(cod3_debug_ngl_side_live_shot_at)));
  if (count <= 0 || frame < at) return false;
  const uint64_t since = frame - at, every = DumpEvery();
  return since / every < 3 && since % every < static_cast<uint64_t>(count);
}
// side119: the frames drawn every frame (cod3_ngl_side_live) between the dumps leave out the dumps' log lines
bool g_quiet = false;
// rn42j: the live frame being drawn is saved (LiveShotFrame): its effects' line is logged too
bool g_live_shot = false;
// side124: the GPU emulation is off and the native picture is the one in the window (StartNativeVulkan)
std::atomic<bool> g_native{false};
// Every frame is drawn natively: in the window (cod3_native_vulkan) or beside the emulated GPU (cod3_ngl_side_live;
// [rexglue-native] 2026-11-09 rn33: or for the live compare, render/native_compare.h)
bool Live() {
  return g_native.load(std::memory_order_relaxed) || REXCVAR_GET(cod3_ngl_side_live) || cod3::compare::Wanted();
}
// The game's draws are watched (the sky's, the effects', the gun's light) for the dumps or for the live picture
bool Watching() { return REXCVAR_GET(cod3_debug_ngl_side) || Live(); }
constexpr uint32_t kMaxScenes = 256;
constexpr size_t kMaxNodes = 20000;
constexpr size_t kMaxTextureBytes = 32u << 20;
// side153: the end of the GPU's memory (512 MB). A texture's surface counted with its padding (pitch and rows rounded up
// to 32 blocks) can run past it when the texture sits at its very end: mace2 ended twice reading 0x20029000 and
// 0x20038000 past it. Every texture read stops there.
constexpr size_t kPhysicalBytes = 0x20000000u;
size_t PhysicalRoom(size_t physical) { return physical < kPhysicalBytes ? kPhysicalBytes - physical : 0; }
constexpr size_t kMaxText = 1024;
constexpr int kListedQuads = 48;  // quads and strings listed by the first big dump
// the generic surface node and what it points to (see the top)
constexpr uint32_t kNodeObject = 0x0C;
constexpr uint32_t kNodeSurface = 0x10;
constexpr uint32_t kNodeMaterial = 0x14;
constexpr uint32_t kSurfaceBoxLow = 0x10;
constexpr uint32_t kSurfaceBoxHigh = 0x20;
constexpr uint32_t kSurfaceBuffers = 0x40;  // two of {Direct3D buffer, start, count, index size}, 0x10 bytes apart
constexpr uint32_t kSurfaceFormat = 0x60;
constexpr uint32_t kSurfaceStride = 0x70;
constexpr uint32_t kBufferFetch = 0x18;
constexpr uint32_t kSceneProjection = 0x130;
constexpr uint32_t kSceneViewProjection = 0x1B0;
constexpr int kShownSurfaces = 6;   // generic surfaces shown in full, once
constexpr size_t kShownScenes = 6;  // 3D scenes shown (their cameras), once each
constexpr uint32_t kMaxDraws = 16384;

bool Pointer(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// A physical address (as fetch constants hold them) as the guest sees it; others are already guest addresses.
uint32_t GuestAddress(uint32_t address) { return address < 0x20000000u ? (0xA0000000u | address) : address; }

// Inside the guest's views of physical memory (0xA0000000 and up), which are mapped whole, so reading is safe.
bool Physical(uint32_t address) { return address >= 0xA0000000u && address < 0xFFF00000u; }

// Physical memory or the program's own image (code and data, 0x82000000 to the globals past 0x82900000), where some
// materials keep their sampler lists.
bool Readable(uint32_t address) {
  return !(address & 3) && (Physical(address) || (address >= 0x82000000u && address < kImageEnd));
}

// side138: the GPU's data (what a fetch constant's physical address names, seen here at 0xA0000000 and up) is read
// through the host's whole mapping of physical memory, as the GPU emulation reads it. The guest's 0xA0000000 view of a
// page can be unmapped when the game allocated it through another view: saint_lo's and fuelplnt's grass blades (read at
// A539C000) ended the game in side137.
[[maybe_unused]] uint8_t* PhysicalBase() {
#if defined(COD3_NGL_VK)
  static std::atomic<uint8_t*> physical{nullptr};
  uint8_t* p = physical.load(std::memory_order_acquire);
  if (!p) {
    auto* kernel = REX_KERNEL_STATE();
    p = kernel && kernel->memory() ? kernel->memory()->physical_membase() : nullptr;
    physical.store(p, std::memory_order_release);
  }
  return p;
#else
  return nullptr;
#endif
}

// A pointer to the GPU data at a 0xA0000000-view address (others as the guest sees them).
[[maybe_unused]] uint8_t* GpuPointer(uint8_t* base, uint32_t address) {
  uint8_t* physical = address >= 0xA0000000u && address < 0xC0000000u ? PhysicalBase() : nullptr;
  return physical ? physical + (address & 0x1FFFFFFFu) : base + address;
}

[[maybe_unused]] uint32_t GpuU32(uint8_t* base, uint32_t address) {
  uint32_t word;
  std::memcpy(&word, GpuPointer(base, address), 4);
  return __builtin_bswap32(word);  // guest memory is big-endian
}

[[maybe_unused]] uint16_t GpuU16(uint8_t* base, uint32_t address) {
  uint16_t half;
  std::memcpy(&half, GpuPointer(base, address), 2);
  return __builtin_bswap16(half);
}

// (side164: through GpuU32, as a 0xA0000000-view page can be unmapped in the guest's view; see PhysicalBase)
std::string Words(uint8_t* base, uint32_t at, uint32_t count) {
  std::string text;
  char item[12];
  for (uint32_t k = 0; k < count; ++k) {
    std::snprintf(item, sizeof item, "%s%08X", k ? " " : "", GpuU32(base, at + 4 * k));
    text += item;
  }
  return text;
}

#if defined(COD3_NGL_VK)
// ---------------------------------------------------------------------------------------------------------------
// Xenos textures to BGRA8.

struct Format {
  uint32_t block;   // texels across a block (1 or 4)
  uint32_t bytes;   // bytes a block
  uint32_t log2_bytes;
};

bool FormatOf(uint32_t format, Format* f) {
  switch (format) {
    case 2:   // k_8 (side141: one channel a texel, as hostage's smoke's k_8_A: drawn as flat squares without it)
    case 8:   // k_8_A
    case 9:   // k_8_B
      *f = {1, 1, 0};
      return true;
    case 10:  // k_8_8
    case 3:   // k_1_5_5_5 (side198: the 16-bit packed kinds, unpacked in DecodeSurface)
    case 4:   // k_5_6_5
    case 5:   // k_6_5_5
    case 15:  // k_4_4_4_4
      *f = {1, 2, 1};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16 (the same texels, filtered wider)
    case 7:   // k_2_10_10_10 (side198: the 32-bit packed kinds, unpacked in DecodeSurface)
    case 54:  // k_2_10_10_10_AS_16_16_16_16
    case 16:  // k_10_11_11
    case 55:  // k_10_11_11_AS_16_16_16_16
    case 17:  // k_11_11_10
    case 56:  // k_11_11_10_AS_16_16_16_16
      *f = {1, 4, 2};
      return true;
    case 18:  // k_DXT1
    case 51:  // k_DXT1_AS_16_16_16_16
    case 58:  // k_DXT3A (one channel, four bits a texel)
    case 59:  // k_DXT5A (one channel, a BC4 block)
      *f = {4, 8, 3};
      return true;
    case 19:  // k_DXT2_3
    case 20:  // k_DXT4_5
    case 52:  // k_DXT2_3_AS_16_16_16_16
    case 53:  // k_DXT4_5_AS_16_16_16_16
    case 49:  // k_DXN (two channels, two BC4 blocks)
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

void SwapBlock(uint8_t* b, uint32_t bytes, uint32_t endian) {
  for (uint32_t i = 0; i + 3 < bytes; i += 4) {
    uint8_t* w = b + i;
    switch (endian) {
      case 1:  // 8 in 16
        std::swap(w[0], w[1]);
        std::swap(w[2], w[3]);
        break;
      case 2:  // 8 in 32
        std::swap(w[0], w[3]);
        std::swap(w[1], w[2]);
        break;
      case 3:  // 16 in 32
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

// A BC4 block (eight bytes: two ends, then 3-bit picks) into one channel of 16 texels.
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
  if (format == 58 || format == 59 || format == 49) {  // side141: one or two channels; the others 0, 0, 1 as sampled
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
    for (int t = 0; t < 16; ++t) {
      const uint32_t a = (b[t / 2] >> (4 * (t & 1))) & 15;
      texels[t][3] = static_cast<uint8_t>(a * 17);
    }
  } else {  // 20
    DecodeColors(b + 8, true, texels);
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
    for (int t = 0; t < 16; ++t) texels[t][3] = static_cast<uint8_t>(a[(bits >> (3 * t)) & 7]);
  }
}

uint32_t Log2Ceil(uint32_t v) {
  uint32_t l = 0;
  while (l < 31 && (1u << l) < v) ++l;
  return l;
}

// side198: a packed texel's channels x, y, z, w (as the GPU numbers them, x in the low bits) as bytes: each field's
// bits, low first; a format with no w field gets 255 there.
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
// surface); each texel's channels picked by the fetch constant's swizzle.
void DecodeSurface(const uint8_t* source, size_t bytes, const Format& f, uint32_t format, uint32_t endian,
                   uint32_t swizzle, bool tiled, uint32_t pitch_blocks, uint32_t offset_x, uint32_t offset_y,
                   uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  bgra.assign(static_cast<size_t>(width) * height * 4, 0);
  uint8_t block[16];
  uint8_t texels[16][4];
  PackedFields packed{};
  const bool is_packed = PackedFormat(format, &packed);  // side198
  for (uint32_t by = 0; by < blocks_y; ++by) {
    const int32_t sy = static_cast<int32_t>(by + offset_y);
    const int32_t row = tiled ? TiledRow(sy, static_cast<int32_t>(pitch_blocks), f.log2_bytes) : 0;
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const int32_t sx = static_cast<int32_t>(bx + offset_x);
      const uint32_t index = tiled ? static_cast<uint32_t>(TiledOffset(sx, sy, f.log2_bytes, row)) >> f.log2_bytes
                                   : static_cast<uint32_t>(sy) * pitch_blocks + static_cast<uint32_t>(sx);
      if (static_cast<size_t>(index) * f.bytes + f.bytes > bytes) continue;
      if (f.bytes < 4) {  // side141: one or two bytes a texel: each byte found through the 32-bit word swap
        const size_t at = static_cast<size_t>(index) * f.bytes;
        const size_t flip = endian == 1 ? 1u : endian == 2 ? 3u : endian == 3 ? 2u : 0u;  // 8in16, 8in32, 16in32
        uint8_t v[2] = {0, 0};
        for (uint32_t k = 0; k < f.bytes; ++k) {
          const size_t from = (at + k) ^ flip;
          v[k] = from < bytes ? source[from] : uint8_t{0};
        }
        if (is_packed) {  // side198: the 16-bit word, x in its low bits
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
        if (is_packed) {  // side198: the 32-bit word as the GPU reads it (after the swap, low byte first)
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
          // the fetch constant's swizzle picks each channel: 0-3 a channel, 4 zero, 5 one
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

// A Direct3D texture's top level as BGRA8 rows; false (and the reason) when it can't be read.
// A Direct3D texture's fetch constant: the six words the GPU takes it by (+0x1C in the texture object, or a fetch
// constant of the device's for a draw's textures, side85).
bool ReadFetch(uint8_t* base, uint32_t d3d, uint32_t fetch[6]) {
  if (!Pointer(d3d)) return false;
  for (int i = 0; i < 6; ++i) fetch[i] = g::ReadU32(base, d3d + kFetchConstant + 4 * i);
  return true;
}

// side185: how the GPU reads a texture beyond its format, which DecodeFetch doesn't apply: each channel's sign (the
// fetch constant's dword 0 bits 2-9, two bits a channel: 0 unsigned, 1 signed, 2 unsigned biased, 3 gamma, read as
// gamma-encoded and turned linear) and the fetch exponent (dword 3 bits 13-18, signed: what it fetches times
// 2^this). To see whether the sky (natively about a third darker on every level) is read another way: a count by
// kind, and the first textures that aren't plain, logged.
void NoteFetchKind(const uint32_t* fetch, uint32_t width, uint32_t height) {
  static std::atomic<int> plain{0}, gamma{0}, other{0}, shown{0};
  const uint32_t signs = (fetch[0] >> 2) & 0xFF;
  const int raw = static_cast<int>((fetch[3] >> 13) & 0x3F);
  const int exponent = raw >= 32 ? raw - 64 : raw;
  if (signs == 0 && exponent == 0) {
    plain.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ((signs & 3) == 3 ? gamma : other).fetch_add(1, std::memory_order_relaxed);
  if (shown.fetch_add(1, std::memory_order_relaxed) < 12) {
    REXLOG_INFO("cod3: ngl side: texture {}x{} format {} at {:08X}: fetch signs x {} y {} z {} w {}, exponent {} (so "
                "far {} plain, {} gamma, {} other)",
                width, height, fetch[1] & 0x3F, fetch[1] & 0x1FFFF000, signs & 3, (signs >> 2) & 3, (signs >> 4) & 3,
                signs >> 6, exponent, plain.load(), gamma.load(), other.load());
  }
}

bool DecodeFetch(uint8_t* base, const uint32_t* fetch, std::vector<uint8_t>& bgra, uint32_t& width,
                 uint32_t& height, std::string& why) {
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  width = (fetch[2] & 0x1FFF) + 1;
  height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  const bool packed = ((fetch[5] >> 11) & 1) != 0;
  NoteFetchKind(fetch, width, height);  // side185
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
  DecodeSurface(GpuPointer(base, 0xA0000000u | physical), std::min(bytes, PhysicalRoom(physical)), f, format, endian,
                swizzle, tiled, pitch_blocks, offset_x, offset_y, width, height, bgra);
  return true;
}

// [rexglue-native] 2026-11-09 rn12: a texture's memory read as a width x height picture from a given place (in blocks),
// with its fetch constant's format, tiling and pitch: a diagnostic, to see a small packed texture's whole packed tile
// (rn11: the 16x16 DXT1 effect texture 10444000 decodes as a framed square, the white-edged squares in The Mace).
bool DecodeFetchAt(uint8_t* base, const uint32_t* fetch, uint32_t offset_x, uint32_t offset_y, uint32_t width,
                   uint32_t height, std::vector<uint8_t>& bgra) {
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  Format f;
  if (!FormatOf(format, &f) || physical == 0) return false;
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  uint32_t pitch_blocks = std::max(pitch / f.block, blocks_x + offset_x);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const uint32_t rows = tiled ? (blocks_y + offset_y + 31) & ~31u : blocks_y + offset_y;
  const size_t bytes = static_cast<size_t>(pitch_blocks) * rows * f.bytes;
  if (bytes > kMaxTextureBytes) return false;
  DecodeSurface(GpuPointer(base, 0xA0000000u | physical), std::min(bytes, PhysicalRoom(physical)), f, format, endian,
                swizzle, tiled, pitch_blocks, offset_x, offset_y, width, height, bgra);
  return true;
}

bool Decode(uint8_t* base, uint32_t d3d, std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
            std::string& why) {
  uint32_t fetch[6];
  if (!ReadFetch(base, d3d, fetch)) {
    why = "no Direct3D texture";
    return false;
  }
  return DecodeFetch(base, fetch, bgra, width, height, why);
}

// The game's own smaller levels of a Direct3D texture (1 to its last), BGRA8 rows each, from where the GPU keeps them
// (acct2-decomp's for-acct2-bro/TEXTURE-LEVELS.md and tools/cod_convert/xtex.py layout(), checked on 7,169 of the
// game's textures and in the game): after the top level each level is a tiled surface of its own, pitch and rows
// rounded up to 32 blocks, until the first level whose shorter side is 16 texels or less; in a packed texture that
// level and every smaller one share one surface, each at GetPackedMipOffset's place. Levels 1 on start at the fetch
// constant's mip address when it has one (side75: soldiers' far levels came out grey when they were taken from right
// after the top level), else right after the top level. Only tiled 2D textures whose sides are powers of two; for
// others `levels` stays empty (the top level alone, as before).
void DecodeLevelsFetch(uint8_t* base, const uint32_t* fetch, std::vector<std::vector<uint8_t>>& levels) {
  levels.clear();
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  const uint32_t last = (fetch[4] >> 6) & 0xF;
  const uint32_t mip_address = fetch[5] & 0x1FFFF000;
  const bool tiled = (fetch[0] >> 31) != 0, packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if (!tiled || !last || !physical || !FormatOf(format, &f) || (width & (width - 1)) || (height & (height - 1))) return;
  const uint32_t log2_w = Log2Ceil(width), log2_h = Log2Ceil(height);
  const uint32_t first_packed = std::min(log2_w, log2_h) > 4 ? std::min(log2_w, log2_h) - 4 : 0;
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
      if (step < 3) {
        (log2_w > log2_h ? y : x) = 16u >> step;
      } else {
        (log2_w > log2_h ? x : y) = (1u << ((log2_w > log2_h ? log2_w : log2_h) - first_packed)) >> (step - 2);
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
    if (level == 0) continue;  // the top level is Decode's
    const size_t bytes = static_cast<size_t>(pitch) * rows * f.bytes;
    // a surface shared with the top level is at the base; every other is counted from where levels 1 on start
    const uint32_t start = (shared && first_packed == 0) ? physical
                           : mip_address ? mip_address : physical + static_cast<uint32_t>(top_bytes);
    const size_t from = (shared && first_packed == 0) ? at : at - top_bytes;
    if (from + bytes > kMaxTextureBytes || !PhysicalRoom(static_cast<size_t>(start) + from)) {
      levels.clear();
      return;
    }
    static int starts_shown = 0;  // diagnostics: where levels 1 on are, beside right after the top level
    if (level == 1 && starts_shown < 16) {
      ++starts_shown;
      REXLOG_INFO("cod3: ngl side: texture levels: base {:08X}, {}x{} format {}, {} levels, mip address {:08X}, top "
                  "level's end {:08X}",
                  physical, width, height, format, last + 1, mip_address, physical + static_cast<uint32_t>(top_bytes));
    }
    levels.emplace_back();
    DecodeSurface(GpuPointer(base, 0xA0000000u | start) + from,
                  std::min(bytes, PhysicalRoom(static_cast<size_t>(start) + from)), f, format, endian, swizzle, true, pitch,
                  x / f.block, y / f.block, w, h, levels.back());
  }
}

// Smaller levels for a picture that has none of the game's (from an HD or user pack): each the level above averaged
// 2 x 2, down to 1 x 1. Sides that are powers of two only; otherwise none.
void BoxLevels(const std::vector<uint8_t>& top, uint32_t width, uint32_t height,
               std::vector<std::vector<uint8_t>>& levels) {
  levels.clear();
  if ((width & (width - 1)) || (height & (height - 1))) return;
  const std::vector<uint8_t>* above = &top;
  uint32_t w = width, h = height;
  while (w > 1 || h > 1) {
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint8_t> level(static_cast<size_t>(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
      for (uint32_t x = 0; x < nw; ++x) {
        for (int c = 0; c < 4; ++c) {
          uint32_t sum = 0;
          for (uint32_t dy = 0; dy < 2; ++dy) {
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const uint32_t sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
              sum += (*above)[(static_cast<size_t>(sy) * w + sx) * 4 + c];
            }
          }
          level[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
        }
      }
    }
    levels.push_back(std::move(level));
    above = &levels.back();
    w = nw;
    h = nh;
  }
}

// A cut-out's smaller levels keep the top level's share of texels at alpha 128 or over (as xtex.py import's
// --alpha auto): each level's alpha is scaled so that the same share passes the cut. The game's own levels thin like
// a box filter (acct2-decomp 17:06: Saint-Lô's net goes from 28 % to 0 by 16 x 16), so far away a cut-out would
// fade out.
void KeepCoverage(const std::vector<uint8_t>& top, std::vector<std::vector<uint8_t>>& levels) {
  size_t passing = 0;
  for (size_t at = 3; at < top.size(); at += 4) passing += top[at] >= 128;
  const double share = top.empty() ? 0.0 : static_cast<double>(passing) / static_cast<double>(top.size() / 4);
  if (share <= 0.0) return;
  for (std::vector<uint8_t>& level : levels) {
    std::vector<uint8_t> alpha;
    for (size_t at = 3; at < level.size(); at += 4) alpha.push_back(level[at]);
    if (alpha.empty()) continue;
    const size_t keep = std::max<size_t>(1, static_cast<size_t>(share * static_cast<double>(alpha.size()) + 0.5));
    std::nth_element(alpha.begin(), alpha.begin() + static_cast<std::ptrdiff_t>(keep - 1), alpha.end(),
                     std::greater<uint8_t>());
    const uint8_t cut = alpha[keep - 1];  // the keep-th largest alpha
    if (cut == 0) continue;
    const double scale = 128.0 / cut;
    for (size_t at = 3; at < level.size(); at += 4) {
      level[at] = static_cast<uint8_t>(std::min(255.0, std::floor(level[at] * scale + 0.5)));
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The HD and user texture packs on the native path (owner 15:11: "make sure you know about the cod3 hd textures and
// the originals"). The setup tool rips every texture (data/cod3/textures/, hd_keys.tsv), upscales the color maps
// into data/cod3/hd_textures/ and builds the player's own pictures into data/cod3/user_textures/
// (docs/TEXTURE_REPLACEMENT.md, game/hd_textures.h). The GPU emulation swaps them in (patch 0016); ngl_vk is handed
// the game's own texture here, so the same lookup is made: the same key (patch 0016's ComputeKey), the same index
// lines, the user pack over the HD pack, the original's alpha kept. Only DXT1/3/5 for now (the color maps); a
// multiplayer match takes no replacements on this path yet.

std::filesystem::path ScreenshotFolder();  // below: <game folder>/userdata/logs/screenshots

// XXH64 with seed 0, the packs' hash (checked against the published vectors: "" EF46DB3751D8E999, "abc"
// 44BC2CF5AD770999).
uint64_t Xxh64(const uint8_t* p, size_t n) {
  constexpr uint64_t k1 = 11400714785074694791ull, k2 = 14029467366897019727ull, k3 = 1609587929392839161ull,
                     k4 = 9650029242287828579ull, k5 = 2870177450012600261ull;
  auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
  auto read64 = [](const uint8_t* q) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | q[i];
    return v;
  };
  auto step = [&](uint64_t acc, uint64_t v) { return rotl(acc + v * k2, 31) * k1; };
  size_t i = 0;
  uint64_t h;
  if (n >= 32) {
    uint64_t v[4] = {k1 + k2, k2, 0, 0ull - k1};
    for (; i + 32 <= n; i += 32) {
      for (int lane = 0; lane < 4; ++lane) v[lane] = step(v[lane], read64(p + i + 8 * lane));
    }
    h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
    for (const uint64_t lane : v) h = (h ^ step(0, lane)) * k1 + k4;
  } else {
    h = k5;
  }
  h += n;
  for (; i + 8 <= n; i += 8) h = rotl(h ^ step(0, read64(p + i)), 27) * k1 + k4;
  if (i + 4 <= n) {
    const uint64_t word =
        uint64_t(p[i]) | uint64_t(p[i + 1]) << 8 | uint64_t(p[i + 2]) << 16 | uint64_t(p[i + 3]) << 24;
    h = rotl(h ^ (word * k1), 23) * k2 + k3;
    i += 4;
  }
  for (; i < n; ++i) h = rotl(h ^ (p[i] * k5), 11) * k1;
  h ^= h >> 33;
  h *= k2;
  h ^= h >> 29;
  h *= k3;
  return h ^ (h >> 32);
}

// A texture's pack key: its top level's blocks gathered into rows without padding (from inside its packed mip tile
// when the smaller side is 16 or less), byte-swapped as the GPU reads them, hashed. As patch 0016's ComputeKey: the
// pitch is at least the width, and a tiled pitch is aligned to 32 blocks.
bool TextureKeyFetch(uint8_t* base, const uint32_t* fetch, uint64_t& key, uint32_t& format_out) {
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const bool packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if (!FormatOf(format, &f) || physical == 0) return false;
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  uint32_t offset_x = 0, offset_y = 0;
  if (packed && std::min(Log2Ceil(width), Log2Ceil(height)) <= 4) {
    (Log2Ceil(width) > Log2Ceil(height) ? offset_y : offset_x) = 16 / f.block;
  }
  uint32_t pitch_blocks = std::max(pitch / f.block, blocks_x);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const size_t span = std::min(static_cast<size_t>(pitch_blocks) * (((blocks_y + offset_y) + 31) & ~31u) * f.bytes,
                               PhysicalRoom(physical));
  if (span > kMaxTextureBytes) return false;
  const uint8_t* source = GpuPointer(base, 0xA0000000u | physical);
  std::vector<uint8_t> gathered(static_cast<size_t>(blocks_x) * blocks_y * f.bytes);
  uint8_t* out = gathered.data();
  for (uint32_t by = 0; by < blocks_y; ++by) {
    const int32_t sy = static_cast<int32_t>(by + offset_y);
    const int32_t row = tiled ? TiledRow(sy, static_cast<int32_t>(pitch_blocks), f.log2_bytes) : 0;
    for (uint32_t bx = 0; bx < blocks_x; ++bx, out += f.bytes) {
      const int32_t sx = static_cast<int32_t>(bx + offset_x);
      const size_t at = tiled ? static_cast<size_t>(TiledOffset(sx, sy, f.log2_bytes, row))
                              : (static_cast<size_t>(sy) * pitch_blocks + static_cast<size_t>(sx)) * f.bytes;
      if (at + f.bytes > span) return false;
      std::memcpy(out, source + at, f.bytes);
      SwapBlock(out, f.bytes, endian);
    }
  }
  key = Xxh64(gathered.data(), gathered.size());
  format_out = format;
  return true;
}

bool TextureKey(uint8_t* base, uint32_t d3d, uint64_t& key, uint32_t& format_out) {
  uint32_t fetch[6];
  return ReadFetch(base, d3d, fetch) && TextureKeyFetch(base, fetch, key, format_out);
}

struct PackEntry {
  std::filesystem::path file;
  uint32_t width, height, format;
  bool user;
};
std::unordered_map<uint64_t, PackEntry> g_pack;
bool g_pack_read = false;
size_t g_pack_bytes = 0;                               // BGRA bytes handed to ngl_vk from the packs
constexpr size_t kPackBudget = size_t(1536) << 20;     // beyond it, the game's own textures
constexpr uint32_t kPackSide = 2048;                    // the largest side taken (a smaller mip of a larger file)

// A pack's file name is plain: relative, no "..", no backslash, drive letter or control character (as the SDK's
// IsPlainPackFile).
bool PlainPackFile(const std::string& name) {
  if (name.empty() || name.size() > 255 || name[0] == '/') return false;
  for (const char c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':') return false;
  }
  return name.find("..") == std::string::npos;
}

void ReadPack(const std::filesystem::path& folder, bool user) {
  std::error_code error;
  if (std::filesystem::file_size(folder / "index.tsv", error) > (size_t(64) << 20) || error) return;
  std::ifstream index(folder / "index.tsv");
  std::string line;
  int taken = 0;
  while (std::getline(index, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> field;
    size_t from = 0;
    for (size_t tab; (tab = line.find('\t', from)) != std::string::npos; from = tab + 1) {
      field.push_back(line.substr(from, tab - from));
    }
    field.push_back(line.substr(from));
    if (field.size() < 5 || field[0].size() != 16 || !PlainPackFile(field[4])) continue;
    char* end = nullptr;
    const uint64_t key = std::strtoull(field[0].c_str(), &end, 16);
    if (*end) continue;
    PackEntry entry{folder / field[4], static_cast<uint32_t>(std::strtoul(field[1].c_str(), nullptr, 10)),
                    static_cast<uint32_t>(std::strtoul(field[2].c_str(), nullptr, 10)),
                    static_cast<uint32_t>(std::strtoul(field[3].c_str(), nullptr, 10)), user};
    g_pack[key] = entry;  // the user pack is read second, so its file wins
    ++taken;
  }
  REXLOG_INFO("cod3: ngl side: {} pack: {} textures ({})", user ? "user" : "HD", taken, folder.string());
}

void ReadPacks() {
  if (g_pack_read) return;
  g_pack_read = true;
  const std::filesystem::path root = ScreenshotFolder().parent_path().parent_path().parent_path();
  if (GetHdTextures() == HdTextures::kOn) ReadPack(root / "data" / "cod3" / "hd_textures", false);
  if (hd::GetUserTextures() == HdTextures::kOn) ReadPack(root / "data" / "cod3" / "user_textures", true);
  if (g_pack.empty()) {
    REXLOG_INFO("cod3: ngl side: no texture pack in use (HD textures: {}; user textures: {})",
                DescribeHdTextures(GetHdTextures()), DescribeHdTextures(hd::GetUserTextures()));
  }
}

// A pack's DDS file as BGRA rows, its largest level no larger than kPackSide a side, the guest's swizzle applied as
// Decode does; false (and the reason) when it can't be used. Every header value is checked before use.
bool DecodePackFile(const PackEntry& entry, uint32_t swizzle, std::vector<uint8_t>& bgra, uint32_t& width,
                    uint32_t& height, std::string& why) {
  static const char* kFourCc[3] = {"DXT1", "DXT3", "DXT5"};  // formats 18, 19, 20
  if (entry.format < 18 || entry.format > 20) {
    why = "format " + std::to_string(entry.format) + " not taken natively yet";
    return false;
  }
  std::ifstream file(entry.file, std::ios::binary);
  uint8_t header[128];
  if (!file.read(reinterpret_cast<char*>(header), sizeof header)) {
    why = "can't read";
    return false;
  }
  auto u32 = [&](int at) {
    return uint32_t(header[at]) | uint32_t(header[at + 1]) << 8 | uint32_t(header[at + 2]) << 16 |
           uint32_t(header[at + 3]) << 24;
  };
  const char* four_cc = kFourCc[entry.format - 18];
  if (std::memcmp(header, "DDS ", 4) || u32(4) != 124 || u32(76) != 32 || std::memcmp(header + 84, four_cc, 4)) {
    why = "not a plain DDS of the original's format";
    return false;
  }
  uint32_t w = u32(16), h = u32(12);
  const uint32_t levels = std::max(1u, u32(28));
  if (!w || !h || w > 16384 || h > 16384 || levels > 15) {
    why = "size out of range";
    return false;
  }
  Format f;
  FormatOf(entry.format, &f);
  auto level_bytes = [&](uint32_t lw, uint32_t lh) { return size_t((lw + 3) / 4) * ((lh + 3) / 4) * f.bytes; };
  size_t skip = 0;
  uint32_t level = 0;
  while (std::max(w, h) > kPackSide && level + 1 < levels) {
    skip += level_bytes(w, h);
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
    ++level;
  }
  if (std::max(w, h) > kPackSide) {
    why = "larger than " + std::to_string(kPackSide) + " without a smaller level";
    return false;
  }
  std::vector<uint8_t> blocks(level_bytes(w, h));
  if (!file.seekg(static_cast<std::streamoff>(sizeof header + skip)) ||
      !file.read(reinterpret_cast<char*>(blocks.data()), static_cast<std::streamsize>(blocks.size()))) {
    why = "shorter than its levels";
    return false;
  }
  width = w;
  height = h;
  bgra.assign(static_cast<size_t>(w) * h * 4, 0);
  uint8_t texels[16][4];
  const uint32_t blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      DecodeBlock(&blocks[(static_cast<size_t>(by) * blocks_x + bx) * f.bytes], entry.format, texels);
      for (uint32_t ty = 0; ty < 4; ++ty) {
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t x = bx * 4 + tx, y = by * 4 + ty;
          if (x >= w || y >= h) continue;
          const uint8_t* t = texels[ty * 4 + tx];
          uint8_t c[4];
          for (int k = 0; k < 4; ++k) {
            const uint32_t s = (swizzle >> (3 * k)) & 7;
            c[k] = s < 4 ? t[s] : s == 5 ? 255 : 0;
          }
          uint8_t* out = &bgra[(static_cast<size_t>(y) * w + x) * 4];
          out[0] = c[2];
          out[1] = c[1];
          out[2] = c[0];
          out[3] = c[3];
        }
      }
    }
  }
  return true;
}

// The pack's picture for a texture whose own top level is `original` (width x height), or false. Never more
// see-through than the original: an opaque original stays opaque, else each texel keeps at least its alpha.
bool PackPicture(uint8_t* base, const uint32_t* fetch, const std::vector<uint8_t>& original,
                 uint32_t original_width, uint32_t original_height, std::vector<uint8_t>& bgra, uint32_t& width,
                 uint32_t& height) {
  ReadPacks();
  if (g_pack.empty() || hd::IsMultiplayerMode() || g_pack_bytes >= kPackBudget) return false;
  uint64_t key;
  uint32_t format;
  if (!TextureKeyFetch(base, fetch, key, format)) return false;
  const auto found = g_pack.find(key);
  if (found == g_pack.end()) return false;
  static int shown = 0;
  std::string why;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  if (found->second.format != format || !DecodePackFile(found->second, swizzle, bgra, width, height, why)) {
    if (why.empty()) why = "the index names another format";
    REXLOG_INFO("cod3: ngl side: {:016x}: can't use {}: {}", key, found->second.file.filename().string(), why);
    return false;
  }
  bool opaque = true;
  for (size_t at = 3; at < original.size() && opaque; at += 4) opaque = original[at] == 255;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t& alpha = bgra[(static_cast<size_t>(y) * width + x) * 4 + 3];
      if (opaque) {
        alpha = 255;
      } else {
        const size_t ox = static_cast<size_t>(x) * original_width / width;
        const size_t oy = static_cast<size_t>(y) * original_height / height;
        alpha = std::max(alpha, original[(oy * original_width + ox) * 4 + 3]);
      }
    }
  }
  g_pack_bytes += bgra.size();
  if (shown++ < 8) {
    REXLOG_INFO("cod3: ngl side: {:016x} {}x{} replaced with {} ({}x{}, {} pack)", key, original_width,
                original_height, found->second.file.filename().string(), width, height,
                found->second.user ? "user" : "HD");
  }
  return true;
}

#endif  // COD3_NGL_VK

// ---------------------------------------------------------------------------------------------------------------
// The frame's quads and strings.

struct Quad {
  float x[4], y[4], u[4], v[4];
  uint32_t color[4];
  uint32_t texture;     // the NGL texture
  uint32_t scene = 0;   // for the listing: the scene, the quad's +0x50..+0x5C (z, flags, blend, its argument)
  uint32_t extra[4] = {};
};

struct Text {  // a string node, for the listing
  uint32_t node, font, color;
  float x, y, scale_x, scale_y;
  int glyphs;
  std::string text;
};

struct Solid {  // a generic surface node (or an old material class's, side90) and its scene
  uint32_t scene, node;
  int old_class = kNotOld;
};

struct Census {
  int scenes = 0, offscreen_scenes = 0, quads = 0, strings = 0, glyphs = 0, other_nodes = 0;
  std::unordered_map<uint32_t, int> others;  // the other nodes by vtable
  std::vector<Text> texts;
  std::vector<Solid> solids;
  std::vector<Solid> trees;                       // SpeedTree nodes (kTreeNode) and their scenes
  std::vector<Solid> grass;                       // grass nodes (kGrassNode) and their scenes
  int far_solids = 0, far_trees = 0;              // of those, from the scenes' third (far) lists; far trees left out
  int old_nodes = 0;                              // of the solids, old material classes' nodes (side90)
  std::unordered_map<uint32_t, int> scene_order;  // the order scenes are drawn in (children first)
  uint32_t world_scene = 0;                       // side176: the first scene drawn with 3D (solids) in it
};

// side91: the first-person gun's objects (the front scene's), for OnPass and OnDraw to probe one of its draws: what
// the GPU is handed there, against what the native side draws it with (the gun comes out wider than the game's,
// though the object's own world-view-projection, +0x40, is what the native side computes).
std::mutex g_front_mutex;
std::unordered_set<uint32_t> g_front_objects;
// side116: the light the game hands a front object's draw (the campaign's model light block, 0x829112A0, read at the
// draw OnPass marks): its main light's color and way, and the ambient its quadratic rows hold (the mean of xx, yy, zz).
// side109 found the gun's main light 0.42 0.38 0.30 with no way set and an ambient of 0.35, where the native side lit
// it by its light set's sun (0.63) and ambient (0.23): darker than the game's on every side the sun doesn't reach.
struct FrontLight {
  float color[3] = {};
  float way[3] = {};
  float ambient[3] = {};
};
std::unordered_map<uint32_t, FrontLight> g_front_lights;  // object -> its last draw's light (g_front_mutex)
// side184: the light the game hands every lit model's draws (the same block, read at each of its draws), kept by
// object for its native draw: the nine irradiance rows (red, green and blue against xx, yy, zz, xy, yz, zx, x, y and
// z of the world normal, in the order IrradianceCoeffs_0..8 take them) and the nearest point light (its place,
// 1 / its radius, its color; all 0 for none)
struct GameModelLight {
  float rows[9][3] = {};
  float point[7] = {};
  uint64_t frame = 0;  // g_frames when it was read
  // rn40j: where the model stood (its world matrix's translation, object +0x30) and its light set (0 for none)
  float at[3] = {};
  bool placed = false;
  uint32_t set = 0;
  // [rexglue-native] 2026-11-10 rn42w: the block's main light (+0x00 its color, the shaders' COD_SHADOW_PROJCOLOR;
  // +0x10 its way, toward the light: for the sun's shadow light the sun's place x 10^6), as the front scene's
  // FrontLight reads it; main is false when there is none (no way, or not numbers)
  float main_color[3] = {};
  float main_way[3] = {};
  bool main = false;
  uint32_t object = 0;  // rn43a: the object it was kept for (its drawn values are kept by it)
};
// rn42w: objects kept; when full, those too old to be used are dropped (rn42u: all were dropped at once every few
// frames, as the objects are made anew at new addresses each frame)
constexpr size_t kMaxModelLights = 4096;
constexpr uint64_t kModelLightFrames = 4;    // how old a light may be for the native draw
std::mutex g_model_light_mutex;
std::unordered_map<uint32_t, GameModelLight> g_model_lights;  // object -> its last lit draw's light
// [rexglue-native] 2026-11-10 rn40j: the same lights kept by where their models stood (cod3_debug_ngl_side_light_by_place):
// a grid of kKeptLightCell game units, each cell the last kKeptLightsPerCell models' lights (g_model_light_mutex)
constexpr float kKeptLightCell = 64.0f;
constexpr float kKeptLightNear = 48.0f;  // how far from a model a light kept for another may be taken
constexpr int kKeptLightsPerCell = 4;
constexpr size_t kMaxKeptLightCells = 4096;  // cells kept (all dropped when full)
struct KeptLightCell {
  uint32_t objects[kKeptLightsPerCell] = {};
  GameModelLight lights[kKeptLightsPerCell];
};
std::unordered_map<uint64_t, KeptLightCell> g_kept_light_cells;
uint64_t KeptLightCellKey(int64_t x, int64_t y, int64_t z) {
  return (static_cast<uint64_t>(x) & 0x1FFFFF) << 42 | (static_cast<uint64_t>(y) & 0x1FFFFF) << 21 |
         (static_cast<uint64_t>(z) & 0x1FFFFF);
}
int64_t KeptLightCellOf(float v) { return static_cast<int64_t>(std::floor(v / kKeptLightCell)); }
// [rexglue-native] 2026-11-10 rn43a: a pass's values as the game drew an object (cod3_debug_ngl_side_drawn_values).
// For each value block (a shader state's +0x1C), the offsets the native side has read from it (ReadBound notes them,
// up to kDrawnOffsets a block); at each lit draw (KeepModelLight) the block's values at those offsets, four floats each,
// kept by object and block. Under g_model_light_mutex.
constexpr uint32_t kDrawnOffsets = 16;
constexpr size_t kMaxDrawnBlocks = 1024;  // blocks whose offsets are noted (all forgotten when full)
constexpr size_t kMaxDrawnValues = 8192;  // objects' kept values; when full, those too old are dropped
struct DrawnOffsets {
  uint32_t count = 0;
  uint32_t offsets[kDrawnOffsets] = {};
};
struct DrawnValues {
  uint64_t frame = 0;  // g_frames when kept
  uint32_t block = 0;  // rn43c: the value block they were read from
  float at[3] = {};    // rn43c: where the model stood (its light's place), when placed
  bool placed = false;
  uint32_t count = 0;
  uint32_t offsets[kDrawnOffsets] = {};
  float values[kDrawnOffsets][4] = {};
};
std::unordered_map<uint32_t, DrawnOffsets> g_drawn_offsets;  // value block -> the offsets read from it
std::unordered_map<uint64_t, DrawnValues> g_drawn_values;     // DrawnValuesKey(object, block) -> its values
uint64_t DrawnValuesKey(uint32_t object, uint32_t block) { return uint64_t{object} << 32 | block; }
// [rexglue-native] 2026-11-10 rn43c: the same values kept by where their models stood, for each block (as the lights,
// rn40j): rn43a's The Mace showed models whose light was taken by place, or from their own address's last model (a
// different part by then), with no values kept for their own block, so their sun was read late (0.773, 0.373) on
// those frames. A grid of kKeptLightCell units per block, each cell the last kKeptLightsPerCell models' values.
constexpr size_t kMaxDrawnCells = 8192;  // (block, cell)s kept (all dropped when full)
struct DrawnCell {
  uint32_t objects[kKeptLightsPerCell] = {};
  DrawnValues values[kKeptLightsPerCell];
};
std::unordered_map<uint64_t, DrawnCell> g_drawn_cells;  // DrawnCellKey(block, cell) -> its models' values
uint64_t DrawnCellKey(uint32_t block, int64_t x, int64_t y, int64_t z) {
  return KeptLightCellKey(x, y, z) ^ (uint64_t{block} * 0x9E3779B97F4A7C15ull);  // (each entry names its block)
}
thread_local uint32_t t_lit_values = 0;  // the value block of the pass being drawn (OnPass), 0 when not known
// [rexglue-native] 2026-11-09 rn25c: lit draws in passes that didn't refill the light block, so not kept (OnDraw)
std::atomic<uint64_t> g_light_block_stale_draws{0};
// [rexglue-native] 2026-11-10 rn40h: the Polish infantry's helmets (5931940F174EA055) have no kept light natively
// (rn40g: the helmet object A8B13900's nine rows all 0, the same soldier's head A8B18C40's there), though the GPU's
// helmet draw has the soldier's rows (rn40d: its c21-c23 = the skin draw's c20-c22). To see which passes and draws the
// game gives a helmet's object: WatchDraw names up to kWatchedLightObjects helmet objects whose maps are watched
// (cod3_debug_ngl_side_watch), with their places (object +0x30), and OnPass / OnDraw log what comes for them, and
// OnPass the passes of other objects near them (rn40i: rn40h's first try watched every helmet and spent its lines on
// the ulans', which have their light). Debug only (cod3_debug_ngl_side); written by the native side, read by the
// render job.
constexpr int kWatchedLightObjects = 8;
constexpr float kWatchedNear = 100.0f;  // how near another object's place counts (game units)
struct WatchedLightObjectPlace {
  uint32_t object = 0;
  float at[3] = {};
};
std::mutex g_watched_light_mutex;
WatchedLightObjectPlace g_watched_light_objects[kWatchedLightObjects];  // (g_watched_light_mutex)
std::atomic<int> g_watched_light_count{0};
void WatchLightObject(uint32_t object, const float at[3]) {
  std::lock_guard<std::mutex> lock(g_watched_light_mutex);
  const int count = g_watched_light_count.load(std::memory_order_relaxed);
  for (int k = 0; k < count; ++k) {
    if (g_watched_light_objects[k].object == object) return;
  }
  if (count == kWatchedLightObjects) return;
  g_watched_light_objects[count].object = object;
  for (int i = 0; i < 3; ++i) g_watched_light_objects[count].at[i] = at[i];
  g_watched_light_count.store(count + 1, std::memory_order_relaxed);
}
// 1: a watched object; 2: near one (its place within kWatchedNear; *near the watched one); 0: neither
// (rn43d: "near" is a macro in the Windows headers, so near_one)
int WatchedLightObject(uint32_t object, const float* at, uint32_t* near_one) {
  if (!object || g_watched_light_count.load(std::memory_order_relaxed) == 0) return 0;
  std::lock_guard<std::mutex> lock(g_watched_light_mutex);
  const int count = g_watched_light_count.load(std::memory_order_relaxed);
  for (int k = 0; k < count; ++k) {
    if (g_watched_light_objects[k].object == object) return 1;
  }
  for (int k = 0; at && k < count; ++k) {
    float distance = 0.0f;
    for (int i = 0; i < 3; ++i) {
      const float delta = at[i] - g_watched_light_objects[k].at[i];
      distance += delta * delta;
    }
    if (distance < kWatchedNear * kWatchedNear) {
      if (near_one) *near_one = g_watched_light_objects[k].object;
      return 2;
    }
  }
  return 0;
}
// [rexglue-native] 2026-11-09 rn28: the diffuse textures (their physical base, as a fetch constant holds it) the native
// side draws as the uniforms' fabric, for the host-draw log to find which pixel shader the game draws them with
// (rn27: none of the soldiers' bodies came through the fabric shader 3BA30872B3955C23, only the view model's sleeves).
// Written by the native side, read by the GPU thread; debug only (cod3_debug_ngl_side).
constexpr uint32_t kFabricDiffuseKept = 32;
std::atomic<uint32_t> g_fabric_diffuse[kFabricDiffuseKept];  // (zero as a global)
std::atomic<uint32_t> g_fabric_diffuse_count{0};
std::mutex g_fabric_diffuse_mutex;  // the writers only
void KeepFabricDiffuse(uint32_t physical) {
  if (!physical) return;
  std::lock_guard<std::mutex> lock(g_fabric_diffuse_mutex);
  const uint32_t count = g_fabric_diffuse_count.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < count; ++i) {
    if (g_fabric_diffuse[i].load(std::memory_order_relaxed) == physical) return;
  }
  if (count == kFabricDiffuseKept) return;
  g_fabric_diffuse[count].store(physical, std::memory_order_relaxed);
  g_fabric_diffuse_count.store(count + 1, std::memory_order_release);
}
// [rexglue-native] 2026-11-09 rn35: the watched color maps (cod3_debug_ngl_side_watch), kept the same way for the
// host-draw log. True when the texture is new to the list (its native draw is logged then).
constexpr uint32_t kWatchedKept = 32;
std::atomic<uint32_t> g_watched_diffuse[kWatchedKept];  // (zero as a global)
std::atomic<uint32_t> g_watched_diffuse_count{0};
std::mutex g_watched_diffuse_mutex;  // the writers only
bool KeepWatchedDiffuse(uint32_t physical) {
  if (!physical) return false;
  std::lock_guard<std::mutex> lock(g_watched_diffuse_mutex);
  const uint32_t count = g_watched_diffuse_count.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < count; ++i) {
    if (g_watched_diffuse[i].load(std::memory_order_relaxed) == physical) return false;
  }
  if (count == kWatchedKept) return false;
  g_watched_diffuse[count].store(physical, std::memory_order_relaxed);
  g_watched_diffuse_count.store(count + 1, std::memory_order_release);
  return true;
}
// side188: the lightmapped world's objects whose material has no color map but an environment map and a normal map
// (island's puddles: "island_env" and "water_ripple01_n"), drawn natively in whatever texture comes first (bright
// cyan), for OnDraw to save the game's shaders for them (cod3_debug_ngl_side)
std::mutex g_water_mutex;
std::unordered_set<uint32_t> g_water_objects;

// An NGL texture: its Direct3D texture (+0x14) carries a texture fetch constant (type 2 in its low two bits).
// Only pointers into the physical views (where the heap and textures are, mapped whole) are followed, as a word that
// isn't a pointer could point anywhere.
bool IsTexture(uint8_t* base, uint32_t texture) {
  if (!Readable(texture)) return false;
  const uint32_t d3d = GpuU32(base, texture + kTextureDirect3D);  // side164: GpuU32, as in Words
  return Readable(d3d) && (GpuU32(base, d3d + kFetchConstant) & 3) == 2;
}

// Every texture a material points to (first 0x60 bytes, and one pointer deep in the first 0x40 bytes of each),
// for the listing: "+14: 1024x512 format 18, +28 -> +04: ...". side164: read through GpuU32: side162's Fuel Plant run
// ended here (cod3_debug_ngl_side on: 5 crash lines, off: 0) reading A539DA48, a pointer one deep whose page the
// guest's 0xA0000000 view doesn't map (side137's grass blades were in the same pages).
std::string MaterialTextures(uint8_t* base, uint32_t material, uint32_t bytes = 0x60) {
  std::string text;
  auto add = [&](const std::string& where, uint32_t texture) {
    const uint32_t d3d = GpuU32(base, texture + kTextureDirect3D);
    char item[96];
    std::snprintf(item, sizeof item, "%s%s: %08X %ux%u format %u", text.empty() ? "" : ", ", where.c_str(), texture,
                  GpuU32(base, texture + kTextureWidth), GpuU32(base, texture + kTextureHeight),
                  GpuU32(base, d3d + kFetchConstant + 4) & 0x3F);
    text += item;
  };
  char where[32];
  for (uint32_t at = 0; at < bytes; at += 4) {
    const uint32_t word = GpuU32(base, material + at);
    if (IsTexture(base, word)) {
      std::snprintf(where, sizeof where, "+%02X", at);
      add(where, word);
    } else if (Physical(word) && !(word & 3) && word != material) {
      for (uint32_t k = 0; k < 0x80; k += 4) {
        const uint32_t inner = GpuU32(base, word + k);
        if (!IsTexture(base, inner)) continue;
        std::snprintf(where, sizeof where, "+%02X -> +%02X", at, k);
        add(where, inner);
      }
    }
  }
  return text.empty() ? "none" : text;
}

// A material's sampler list (material +0x30): after two words, entries of 0x20 bytes: the effect parameter, the NGL
// texture, then the texture's fetch constant (6 words) as the sampler uses it. Seen (side12): for a rock 4 entries,
// a DXT5 normal map, the lightmap (parameter 0x829114E0; a global texture when the surface has none), a DXT3 color
// map, a DXT1; for the terrain the lightmap first (the whole map in one texture), again under 0x829114E0, then a
// DXT1 color map.
constexpr uint32_t kMaterialSamplers = 0x30;
constexpr uint32_t kSamplerFirst = 0x08;
constexpr uint32_t kSamplerBytes = 0x20;
#if defined(COD3_MP)
constexpr uint32_t kLightMapParameter = 0x82A8E3F0;  // (1 of 1)
#else
constexpr uint32_t kLightMapParameter = 0x829114E0;
#endif
constexpr uint32_t kMaxSamplers = 8;

struct Samplers {
  uint32_t color = 0, lightmap = 0;  // NGL textures, 0 for none
  uint32_t looks = 0;                // the set of looks the color map was picked from (side86), 0 for none
  uint32_t env = 0, env_mask = 0;    // side88: a shiny surface's environment map and its specular map
  uint32_t normal = 0;               // side117: its normal map (Normal_Map_Texture, k98_n_Texture, ...)
  uint32_t color_b = 0;              // side195: the terrain's second color map (DiffuseMapB), 0 for none
  // [rexglue-native] 2026-11-09 rn24: the uniforms' fabric (Fabric_Mask_Texture, Normal_Swatch_Texture), 0 for none
  uint32_t fabric_mask = 0, normal_swatch = 0;
  // rn32: the heads' and arms' Noise_Swatch_Texture (c_generic_headsw_s, ...), 0 for none
  uint32_t noise_swatch = 0;
  // rn34: the guns' scratch map (the Springfield's Scratch05_SPEC; never taken as the specular map), 0 for none
  uint32_t scratch = 0;
  // rn36: the vehicles' detail normal map (the tank body's Detail_Texture "v_metal_detail01_n"), 0 for none
  uint32_t detail = 0;
  // rn40e: the helmets' Reflection_Cube_Map_Texture (canyon2), 0 for none
  uint32_t reflection_cube = 0;
};

// side33: a sampler parameter's +0x28 points at its sampler state as the effect's own text ("Texture=<Normal_Map_
// Texture>; AddressU = Wrap; ..."), so the texture variable's name tells the maps apart ("" when there is none).
constexpr uint32_t kParameterSamplerText = 0x28;
std::string SamplerTextureName(uint8_t* base, uint32_t parameter) {
  const uint32_t text = Readable(parameter) ? g::ReadU32(base, parameter + kParameterSamplerText) : 0;
  if (!Pointer(text) || !Readable(text)) return "";
  std::string s;
  for (uint32_t k = 0; k < 160 && Readable((text + k) & ~3u); ++k) {  // Readable wants a word's address
    const uint8_t c = base[text + k];
    if (c < 0x20 || c > 0x7E) break;
    s += static_cast<char>(c);
  }
  const size_t at = s.find("Texture=<"), end = at == std::string::npos ? at : s.find('>', at);
  return end == std::string::npos ? "" : s.substr(at + 9, end - at - 9);
}

std::string Lower(const std::string& name) {
  std::string lower;
  for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower;
}

// Whether a texture variable's name says it is the color map, in any case: Color_Map_Texture, Diffuse_Texture,
// k98_diffuse_Texture, BSpringfield_diff_Texture, bullet_D_tga_Texture (side47: "_d" as a word is the diffuse).
// side195: the terrain's second layer's color map (its texture variable "DiffuseMapB..."; checked before
// ColorMapName, which would take it for the first's).
bool SecondColorMapName(const std::string& name) {
  const std::string lower = Lower(name);
  return lower.find("diffusemapb") != std::string::npos || lower.find("colormapb") != std::string::npos;
}

bool ColorMapName(const std::string& name) {
  const std::string lower = Lower(name);
  for (const char* word : {"color", "colour", "diff", "albedo"}) {
    if (lower.find(word) != std::string::npos) return true;
  }
  const size_t d = lower.find("_d_");
  return d != std::string::npos || (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "_d") == 0);
}

// Whether a texture variable's name says it is something else (side47: the gun's environment map was taken for its
// color map, so it came out gold).
bool OtherMapName(const std::string& name) {
  const std::string lower = Lower(name);
  for (const char* word : {"env", "reflect", "spec", "normal", "_n_", "mask", "noise", "swatch", "bump", "light"}) {
    if (lower.find(word) != std::string::npos) return true;
  }
  return false;
}

// side88: a shiny surface's maps. The guns (side86's logs) name an environment map (Springfield and the bullets:
// "environment_Texture", "env_map_Texture", 128 x 128 DXT1) and specular maps ("springfield_s_Texture",
// "w_mp40_spec_Texture", "Scratch05_SPEC_Texture"); the first of each is taken. The K98's "reflection_tint" is
// neither.
bool EnvMapName(const std::string& name) {
  const std::string lower = Lower(name);
  return lower.find("environment") != std::string::npos || lower.find("env_map") != std::string::npos ||
         lower.compare(0, 4, "env_") == 0;
}

bool SpecularMapName(const std::string& name) {
  const std::string lower = Lower(name);
  return lower.find("spec") != std::string::npos || lower.find("_s_") != std::string::npos ||
         (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "_s") == 0);
}

// [rexglue-native] 2026-11-09 rn25: where a surface's pixel shader constants come from, as the game itself binds them
// (debug only, once per pass, at most 4). Read from the game's own tables, not guessed: the node's technique (+0x1C)
// holds its passes (+0x0C, 0xE0 bytes each; 0x82143F70 draws the first one when there is one), a pass's compiled
// uploads at +0xDC (made by 0x82155A90), and in them the pixel shader's float constants at +0xC0 (+0x100 for the
// unlit variant; 0x82154C08 picks): a SET_CONSTANT packet whose first register x 4 is in +0x1C's low 11 bits, then
// +0x08 vectors of {byte offset into the bases, offset} at +0x20 (0x82155EC0; the bases as ShowGrass reads them:
// slot 1 the state's value block, 2 NGL's globals, 3 0x829118E0, 4 the light block 0x829112A0, 6 0x829124E0). A light
// block offset tells which of 0x82142360's records a constant is (+0x00 the main light, +0x30 the nearest point
// light, +0xF0.. the irradiance rows).
bool GrassReadable(uint32_t a);
std::string InlineName(uint8_t* base, uint32_t at);  // rn41l
std::string DeepName(uint8_t* base, uint32_t at);    // rn41l
void LogPassBinding(uint8_t* base, uint32_t node, const char* what) {
  static std::mutex mutex;
  static std::vector<std::pair<std::string, uint32_t>> seen;  // rn34: at most 4 passes of each kind (was 4 in all)
  const uint32_t technique = GrassReadable(node) ? g::ReadU32(base, node + 0x1C) : 0;
  const uint32_t pass = GrassReadable(technique) ? g::ReadU32(base, technique + 0x0C) : 0;
  const uint32_t compiled = GrassReadable(pass) ? g::ReadU32(base, pass + 0xDC) : 0;
  if (!GrassReadable(compiled)) return;
  {
    std::lock_guard<std::mutex> lock(mutex);
    size_t of_kind = 0;
    for (const auto& kept : seen) {
      if (kept.first != what) continue;
      if (kept.second == pass) return;
      ++of_kind;
    }
    if (of_kind >= 4) return;
    seen.emplace_back(what, pass);
  }
  std::string text;
  char item[64];
  for (uint32_t list : {0xC0u, 0x100u}) {
    const uint32_t at = compiled + list;
    const uint32_t count = g::ReadU32(base, at + 0x08), rows = g::ReadU32(base, at + 0x20);
    const uint32_t first = (g::ReadU32(base, at + 0x1C) & 0x7FF) / 4;
    std::snprintf(item, sizeof item, " | list +%X: %u vectors from register %u:", list, count, first);
    text += item;
    for (uint32_t k = 0; k < count && k < 24 && GrassReadable(rows); ++k) {
      std::snprintf(item, sizeof item, " %u=slot %u+%X", first + k, g::ReadU32(base, rows + 8 * k) / 4,
                    g::ReadU32(base, rows + 8 * k + 4));
      text += item;
    }
  }
  REXLOG_INFO("cod3: ngl side: rn25 {} pass {:08X} (technique {:08X}, {} passes), compiled {:08X}:{}", what, pass,
              technique, g::ReadU32(base, technique + 0x08), compiled, text);
  // [rexglue-native] 2026-11-10 rn41i: a technique of more than one pass (rn41e: the lantern's glass has 2, the first
  // binding no pixel constants): the technique's, the first pass's and its compiled object's words, to find the second
  // pass and the shaders both draw with (debug only: the callers log watched draws)
  const uint32_t passes = g::ReadU32(base, technique + 0x08);
  // rn41p: and a watched draw's technique of one pass too (rn41o: the lanterns' flames, "flames", are a node of their
  // own, drawn by the game's 1DF1E9465C1429B4 strip)
  const bool watched_one = passes == 1 && std::strncmp(what, "watched", 7) == 0;
  if ((passes > 1 || watched_one) && passes < 8 && GrassReadable(technique + 0x40) && GrassReadable(pass + 0x100) &&
      GrassReadable(compiled + 0x40)) {
    REXLOG_INFO("cod3: ngl side: rn41i {} technique {:08X}: {}", what, technique, Words(base, technique, 16));
    REXLOG_INFO("cod3: ngl side: rn41i {} first pass {:08X}: {} | +C0: {}", what, pass, Words(base, pass, 16),
                Words(base, pass + 0xC0, 16));
    REXLOG_INFO("cod3: ngl side: rn41i {} its compiled {:08X}: {}", what, compiled, Words(base, compiled, 16));
    // rn41k: every pass (rn41i: the second starts 0xE0 after the first, its words begin as the first's do; 0x82143F70
    // draws a technique of more passes one pass after another through 0x8213C9F8, without the per-light loop), each
    // with its compiled uploads' four lists (+0x80 and +0x140 the vertex shader's, +0xC0 and +0x100 the pixel
    // shader's; decoded as above)
    auto lists = [base](uint32_t uploads) {
      std::string text;
      char item[64];
      for (uint32_t list : {0x80u, 0xC0u, 0x100u, 0x140u}) {
        const uint32_t at = uploads + list;
        const uint32_t count = g::ReadU32(base, at + 0x08), rows = g::ReadU32(base, at + 0x20);
        const uint32_t first = (g::ReadU32(base, at + 0x1C) & 0x7FF) / 4;
        std::snprintf(item, sizeof item, " | list +%X: %u vectors from register %u:", list, count, first);
        text += item;
        for (uint32_t k = 0; k < count && k < 24 && GrassReadable(rows); ++k) {
          std::snprintf(item, sizeof item, " %u=slot %u+%X", first + k, g::ReadU32(base, rows + 8 * k) / 4,
                        g::ReadU32(base, rows + 8 * k + 4));
          text += item;
        }
      }
      return text;
    };
    for (uint32_t p = 0; p < passes; ++p) {
      const uint32_t at = pass + 0xE0 * p;
      if (!GrassReadable(at + 0xE0)) break;
      const uint32_t uploads = g::ReadU32(base, at + 0xDC);
      REXLOG_INFO("cod3: ngl side: rn41k {} pass {} {:08X}: {} | +C0: {} | compiled {:08X}{}", what, p + 1, at,
                  Words(base, at, 16), Words(base, at + 0xC0, 8), uploads,
                  GrassReadable(uploads + 0x180) ? ": " + Words(base, uploads, 16) + lists(uploads)
                                                 : std::string(" (not yet)"));
    }
    // rn41l: where the second pass's map and time come from. rn41k: the lantern glass's second pass is the game's
    // 1DF1E9465C1429B4, an 8-frame strip played at 24 a second (frame floor(c16.x x 24 + 0.5), 1/8 of the width
    // each; c16.x 42.54 at that draw) read from tf1 = 11A44000, which is not among the material's samplers. Logged:
    // the node's shader state's parameters (+0x34, next +0x0C, code +0x10, value +0x14; COD3-NGL-MAP.md) by name,
    // each value's first floats or the texture it is or points to (its physical base, as the host-draw log shows
    // them); each pass's +0xD8 words and the textures they or one pointer deep point to; the scene's time (+0x420)
    auto texture_text = [base](uint32_t texture) {
      const uint32_t d3d = GpuU32(base, texture + kTextureDirect3D);
      const uint32_t word1 = GpuU32(base, d3d + kFetchConstant + 4);
      const uint32_t name = g::ReadU32(base, texture + kTextureName);
      char item[160];
      std::snprintf(item, sizeof item, "texture %08X %ux%u at %08X/%u flags %X \"%s\"", texture,
                    GpuU32(base, texture + kTextureWidth), GpuU32(base, texture + kTextureHeight), word1 & 0x1FFFF000u,
                    word1 & 0x3F, GpuU32(base, texture + kTextureFlags),
                    Pointer(name) && Readable(name) ? InlineName(base, name + 4).c_str() : "-");
      return std::string(item);
    };
    const uint32_t state = g::ReadU32(base, node + 0x18);
    uint32_t parameter = GrassReadable(state) ? g::ReadU32(base, state + 0x34) : 0;
    std::string text;
    for (int k = 0; k < 32 && Pointer(parameter) && Readable(parameter); ++k) {
      const uint32_t value = g::ReadU32(base, parameter + 0x14);
      const uint32_t inner = Pointer(value) && Readable(value) ? g::ReadU32(base, value) : 0;
      std::string shown;
      if (IsTexture(base, value)) {
        shown = texture_text(value);
      } else if (IsTexture(base, inner)) {
        shown = "-> " + texture_text(inner);
      } else if (Pointer(value) && Readable(value + 12)) {
        char item[96];
        std::snprintf(item, sizeof item, "(%g %g %g %g)", g::ReadF32(base, value), g::ReadF32(base, value + 4),
                      g::ReadF32(base, value + 8), g::ReadF32(base, value + 12));
        shown = item;
      } else {
        shown = "-";
      }
      char head[96];
      std::snprintf(head, sizeof head, " | %s code %02X value %08X ", DeepName(base, parameter).c_str(),
                    g::ReadU32(base, parameter + 0x10), value);
      text += head + shown;
      parameter = g::ReadU32(base, parameter + 0x0C);
    }
    const uint32_t scene = g::ReadU32(base, kCurrentScene);
    REXLOG_INFO("cod3: ngl side: rn41l {} state {:08X}, scene time {}; parameters:{}", what, state,
                Pointer(scene) && Readable(scene + 0x420) ? g::ReadF32(base, scene + 0x420) : -1.0f, text);
    for (uint32_t p = 0; p < passes; ++p) {
      const uint32_t at = pass + 0xE0 * p;
      if (!GrassReadable(at + 0xE0)) break;
      const uint32_t block = g::ReadU32(base, at + 0xD8);
      if (!GrassReadable(block + 0x40)) continue;
      std::string found;
      for (uint32_t w = 0; w < 0x40; w += 4) {
        const uint32_t word = g::ReadU32(base, block + w);
        if (IsTexture(base, word)) {
          found += " +" + std::to_string(w) + ": " + texture_text(word);
        } else if (Readable(word) && Readable(word + 0x40)) {  // rn41n: both ends, read through the GPU view
          for (uint32_t k = 0; k < 0x40; k += 4) {
            const uint32_t deeper = GpuU32(base, word + k);
            if (IsTexture(base, deeper)) found += " +" + std::to_string(w) + "->+" + std::to_string(k) + ": " +
                                                  texture_text(deeper);
          }
        }
      }
      REXLOG_INFO("cod3: ngl side: rn41l {} pass {} +D8 {:08X}: {} | textures:{}", what, p + 1, block,
                  Words(base, block, 16), found.empty() ? std::string(" none") : found);
      // rn41m: rn41l found no NGL texture there (and the shader state has only world and worldit; the second pass's
      // c16.x is the scene's time, 43.0884 both). +D8 holds {B1D80458, 6, a list}: the first word's and the list's
      // words, and any Direct3D texture (a fetch constant of type 2 at +0x1C) or NGL texture they or one pointer deep
      // hold, with its physical base (the second pass's map is at 11A44000, the first's at 11C80000).
      // rn41n: only through Readable (the physical views and the image) and GpuU32: rn41m's GrassReadable let a word
      // at 0x70203D3C through (its 0x40000000-0x80000000 allowance), unmapped, and the ngl worker crashed there
      auto safe = [](uint32_t a, uint32_t span) { return Readable(a) && Readable(a + span); };
      auto is_d3d = [base, safe](uint32_t d3d) {
        return safe(d3d, 0x40) && (GpuU32(base, d3d + kFetchConstant) & 3) == 2;
      };
      auto d3d_text = [base](uint32_t d3d) {
        const uint32_t word1 = GpuU32(base, d3d + kFetchConstant + 4);
        char item[64];
        std::snprintf(item, sizeof item, "d3d %08X at %08X/%u", d3d, word1 & 0x1FFFF000u, word1 & 0x3F);
        return std::string(item);
      };
      const uint32_t head = safe(block, 0x0C) ? GpuU32(base, block) : 0;
      const uint32_t list = safe(block, 0x0C) ? GpuU32(base, block + 8) : 0;
      std::string d3ds;
      char where[48];
      for (uint32_t from : {head, list}) {
        if (!safe(from, 0xC0)) continue;
        for (uint32_t w = 0; w < 0xC0; w += 4) {
          const uint32_t word = GpuU32(base, from + w);
          std::snprintf(where, sizeof where, " %08X+%02X: ", from, w);
          if (IsTexture(base, word)) {
            d3ds += where + texture_text(word);
          } else if (is_d3d(word)) {
            d3ds += where + d3d_text(word);
          } else if (word != from && safe(word, 0x40)) {
            for (uint32_t k = 0; k < 0x40; k += 4) {
              const uint32_t deeper = GpuU32(base, word + k);
              std::snprintf(where, sizeof where, " %08X+%02X->+%02X: ", from, w, k);
              if (IsTexture(base, deeper)) {
                d3ds += where + texture_text(deeper);
              } else if (is_d3d(deeper)) {
                d3ds += where + d3d_text(deeper);
              }
            }
          }
        }
      }
      REXLOG_INFO("cod3: ngl side: rn41m {} pass {} +D8's {:08X}: {} | its list {:08X}: {} / {} / {} | textures:{}", what,
                  p + 1, head, safe(head, 0x40) ? Words(base, head, 16) : std::string("-"), list,
                  safe(list, 0xC0) ? Words(base, list, 16) : std::string("-"),
                  safe(list, 0xC0) ? Words(base, list + 0x40, 16) : std::string("-"),
                  safe(list, 0xC0) ? Words(base, list + 0x80, 16) : std::string("-"),
                  d3ds.empty() ? std::string(" none") : d3ds);
    }
    // rn41o: rn41n found the +0xD8 lists to be the effect's own pass texts (pass 1 "CullMode = CW; ...", pass 2
    // "CullMode = CCW; ..."; the effect standard_7_1), with no texture. Logged now, still looking for pass 2's map
    // (11A44000), and read only through Readable and the GPU view (rn41m):
    //   - each pass's text, as characters (up to 0x280 bytes; anything not printable as '|');
    //   - the material (node +0x14) as the fastfile lays one out (cod3-formats.md: +0x14 the effect, +0x24 the
    //     parameter count, +0x28 a table of 0x24-byte entries: +0x00 the name, +0x10 the type, 11 a texture, +0x14
    //     the value, whose first field is the TEX), its first words, and each entry;
    //   - the shader state's effect objects (+0x20, +0x24): any NGL or Direct3D texture in their first 0x100 bytes or
    //     one pointer deep.
    {
      auto safe_at = [](uint32_t a, uint32_t span) { return Readable(a) && Readable(a + span); };
      auto d3d_is = [base, safe_at](uint32_t d3d) {
        return safe_at(d3d, 0x40) && (GpuU32(base, d3d + kFetchConstant) & 3) == 2;
      };
      auto d3d_line = [base](uint32_t d3d) {
        const uint32_t word1 = GpuU32(base, d3d + kFetchConstant + 4);
        char item[64];
        std::snprintf(item, sizeof item, "d3d %08X at %08X/%u", d3d, word1 & 0x1FFFF000u, word1 & 0x3F);
        return std::string(item);
      };
      for (uint32_t p = 0; p < passes; ++p) {
        const uint32_t at = pass + 0xE0 * p;
        const uint32_t block = safe_at(at, 0xE0) ? GpuU32(base, at + 0xD8) : 0;
        const uint32_t list = safe_at(block, 0x0C) ? GpuU32(base, block + 8) : 0;
        if (!safe_at(list, 0x280)) continue;
        std::string chars;
        for (uint32_t i = 0; i < 0x280; ++i) {
          const uint32_t word = GpuU32(base, list + (i & ~3u));
          const char c = static_cast<char>((word >> (8 * (3 - (i & 3)))) & 0xFF);  // big-endian bytes
          chars += c >= 0x20 && c < 0x7F ? c : '|';
        }
        REXLOG_INFO("cod3: ngl side: rn41o {} pass {} text: {}", what, p + 1, chars);
      }
      const uint32_t material = safe_at(node, 0x18) ? GpuU32(base, node + 0x14) : 0;
      if (safe_at(material, 0x60)) {
        const uint32_t count = GpuU32(base, material + 0x24), table = GpuU32(base, material + 0x28);
        std::string entries;
        for (uint32_t k = 0; k < count && k < 16 && safe_at(table + 0x24 * k, 0x24); ++k) {
          const uint32_t entry = table + 0x24 * k;
          const uint32_t name = GpuU32(base, entry), type = GpuU32(base, entry + 0x10);
          const uint32_t value = GpuU32(base, entry + 0x14);
          const uint32_t first = safe_at(value, 4) ? GpuU32(base, value) : 0;
          std::string shown;
          if (IsTexture(base, first)) {
            shown = texture_text(first);
          } else if (IsTexture(base, value)) {
            shown = texture_text(value);
          } else if (safe_at(value, 0x10)) {
            shown = Words(base, value, 4);
          } else {
            shown = "-";
          }
          char head[160];
          std::snprintf(head, sizeof head, " | %u \"%s\" type %u value %08X: ", k,
                        safe_at(name, 8) ? InlineName(base, name + 4).c_str() : "-", type, value);
          entries += head + shown;
        }
        REXLOG_INFO("cod3: ngl side: rn41o {} material {:08X}: {} / {}; {} parameters at {:08X}:{}", what, material,
                    Words(base, material, 16), Words(base, material + 0x40, 8), count, table, entries);
      }
      for (uint32_t field : {0x20u, 0x24u}) {
        const uint32_t object = safe_at(state, 0x28) ? GpuU32(base, state + field) : 0;
        if (!safe_at(object, 0x100)) continue;
        std::string found;
        char where[48];
        for (uint32_t w = 0; w < 0x100; w += 4) {
          const uint32_t word = GpuU32(base, object + w);
          std::snprintf(where, sizeof where, " +%02X: ", w);
          if (IsTexture(base, word)) {
            found += where + texture_text(word);
          } else if (d3d_is(word)) {
            found += where + d3d_line(word);
          } else if (word != object && safe_at(word, 0x40)) {
            for (uint32_t k = 0; k < 0x40; k += 4) {
              const uint32_t deeper = GpuU32(base, word + k);
              std::snprintf(where, sizeof where, " +%02X->+%02X: ", w, k);
              if (IsTexture(base, deeper)) {
                found += where + texture_text(deeper);
              } else if (d3d_is(deeper)) {
                found += where + d3d_line(deeper);
              }
            }
          }
        }
        REXLOG_INFO("cod3: ngl side: rn41o {} state +{:02X} {:08X}: {} | textures:{}", what, field, object,
                    Words(base, object, 16), found.empty() ? std::string(" none") : found);
      }
    }
  }
}

// side117: a normal map's name (the gun's pixel shader CDBAC6026B525050 samples "Normal_Map"; the materials name
// theirs Normal_Map_Texture, k98_n_Texture, ..._nrm_...; hd-correctness's survey: normalmap_texture, DXT5).
bool NormalMapName(const std::string& name) {
  const std::string lower = Lower(name);
  if (lower.find("swatch") != std::string::npos) return false;  // [rexglue-native] rn24: a detail swatch isn't it
  return lower.find("normal") != std::string::npos || lower.find("_n_") != std::string::npos ||
         lower.find("nrm") != std::string::npos || lower.find("bump") != std::string::npos ||
         (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "_n") == 0);
}

// [rexglue-native] 2026-11-09 rn24: the uniforms' fabric maps (rn21's survey: the soldiers' body materials name
// Fabric_Mask_Texture, Normal_Map_Texture, Normal_Swatch_Texture, Diffuse_Map_Texture and Specular_Map_Texture; the
// heads' Noise_Swatch_Texture is another shader's and not a normal swatch)
bool FabricMaskName(const std::string& name) {
  const std::string lower = Lower(name);
  return lower.find("fabric") != std::string::npos && lower.find("mask") != std::string::npos;
}

bool NormalSwatchName(const std::string& name) {
  const std::string lower = Lower(name);
  return lower.find("swatch") != std::string::npos && lower.find("normal") != std::string::npos;
}

// rn32: the skin's detail map (the heads' Noise_Swatch_Texture; the arms' too, whose shader isn't the heads')
bool NoiseSwatchName(const std::string& name) {
  const std::string lower = Lower(name);
  return lower.find("swatch") != std::string::npos && lower.find("noise") != std::string::npos;
}

// [rexglue-native] 2026-11-09 rn34: the guns' scratch map (the Springfield's Scratch05_SPEC, which also says "spec")
bool ScratchName(const std::string& name) { return Lower(name).find("scratch") != std::string::npos; }

// [rexglue-native] 2026-11-09 rn36: the vehicles' detail normal map (the tank body's Detail_Texture; the lightmapped
// world's DetailMap_Texture says it too, but those draws are lightmapped and never take it)
bool DetailName(const std::string& name) { return Lower(name).find("detail") != std::string::npos; }

// [rexglue-native] 2026-11-10 rn40e: the soldiers' helmets' Reflection_Cube_Map_Texture (canyon2): not taken as an
// environment map (EnvMapName), which would draw them shiny; kept apart for the helmets' shader (ReadLensPass)
bool ReflectionCubeName(const std::string& name) {
  return Lower(name).find("reflection_cube") != std::string::npos;
}

// side49: a soldier's color sampler can hold a set of looks instead of one texture (its uniform or head; the set is
// named at +0x10, "c_..._ng_body_text"): +0x24 how many, +0x28 where the list is (right after the set), unused
// entries the image's default texture (not an NGL texture). Which look a soldier wears isn't read yet, so the first
// one is taken; until now these were drawn in their placeholder tint, which made the uniforms white.
constexpr uint32_t kLookCount = 0x24;
constexpr uint32_t kLookList = 0x28;
constexpr uint32_t kMaxLooks = 16;
uint32_t FirstLook(uint8_t* base, uint32_t set) {
  if (!Readable(set) || !Readable(set + kLookList)) return 0;
  const uint32_t count = g::ReadU32(base, set + kLookCount), looks = g::ReadU32(base, set + kLookList);
  if (count == 0 || count > kMaxLooks) return 0;
  for (uint32_t k = 0; k < count && Readable(looks + 4 * k); ++k) {
    const uint32_t texture = g::ReadU32(base, looks + 4 * k);
    if (IsTexture(base, texture)) return texture;
  }
  return 0;
}

// side86: which look a soldier wears, as the game picks it. A set of looks is an animated NGL texture (flag 4 at +0x18,
// its frames the looks), and the game draws frame F mod count of it: 0x82155000 binds a material's samplers so
// (0x8213EB90 does the same for a single texture). F is set for each surface just before, by 0x820D48F0: from the
// object's render parameters (object +0x84: a 64-bit mask, then a word for each parameter, parameter n at +4 (n + 2))
// when the texture-frame parameter's bit is set (its number is kept at 0x8290F2A4 when the parameters are registered),
// else the scene's own frame (scene +0x424).
constexpr uint32_t kObjectRenderParameters = 0x84;
constexpr uint32_t kSceneTextureFrame = 0x424;
#if defined(COD3_MP)
constexpr uint32_t kTextureFrameParameter = 0x82A8C1B8;  // cod3mp's (mp/globals_sp_to_mp.tsv: 5 of 5 uses)
#else
constexpr uint32_t kTextureFrameParameter = 0x8290F2A4;
#endif
constexpr uint32_t kMaxFrames = 256;

struct TextureFrame {
  uint32_t value = 0;
  bool own = false;  // from the object's render parameters (else the scene's frame)
};

TextureFrame FrameFor(uint8_t* base, uint32_t scene, uint32_t object) {
  TextureFrame frame;
  if (Readable(scene + kSceneTextureFrame)) frame.value = g::ReadU32(base, scene + kSceneTextureFrame);
  const uint32_t bit = g::ReadU32(base, kTextureFrameParameter);  // a global of the image
  const uint32_t parameters =
      Readable(object + kObjectRenderParameters) ? g::ReadU32(base, object + kObjectRenderParameters) : 0;
  if (bit >= 64 || !Readable(parameters) || !Readable(parameters + 4 * (bit + 2))) return frame;
  const uint64_t mask = uint64_t{g::ReadU32(base, parameters)} << 32 | g::ReadU32(base, parameters + 4);
  if ((mask >> bit) & 1) {
    frame.value = g::ReadU32(base, parameters + 4 * (bit + 2));
    frame.own = true;
  }
  return frame;
}

// The look a set's frame picks (frames[F mod count], as 0x82155000 takes it), or the first look that is a texture when
// that entry isn't one (an unused entry) or the set isn't flagged animated; 0 when there is none.
uint32_t LookFor(uint8_t* base, uint32_t set, uint32_t frame) {
  if (!Readable(set) || !Readable(set + kLookList)) return 0;
  if (!(g::ReadU32(base, set + kTextureFlags) & kTextureAnimated)) return FirstLook(base, set);
  const uint32_t count = g::ReadU32(base, set + kLookCount), looks = g::ReadU32(base, set + kLookList);
  if (count == 0 || count > kMaxFrames || !Readable(looks + 4 * (frame % count))) return FirstLook(base, set);
  const uint32_t texture = g::ReadU32(base, looks + 4 * (frame % count));
  return IsTexture(base, texture) ? texture : FirstLook(base, set);
}

std::string InlineName(uint8_t* base, uint32_t at);

// side156: whether a texture handed under the lightmap's parameter is a lightmap. The world's are named lm0001 and so
// on; the vehicles' materials hand a detail normal map there ("v_metal_detail01_n", side155's survey), which taken as
// their light turned the jeeps and the trucks' insides near white. A texture whose own name says it is a normal,
// color, specular or detail map isn't one.
bool LightmapTextureByName(uint8_t* base, uint32_t name);
uint32_t TextCheck(uint8_t* base, uint32_t text);

bool LightmapTexture(uint8_t* base, uint32_t texture) {
  if (!IsTexture(base, texture) || !Physical(texture)) return false;
  const uint32_t name = g::ReadU32(base, texture + kTextureName);
  if (!Pointer(name) || !Readable(name)) return true;
  // side208: worked out once per texture and name (checked by the name's first words), not for every draw
  struct Entry {
    uint32_t name, check;
    bool lightmap;
  };
  static std::unordered_map<uint32_t, Entry> known;
  const uint32_t check = TextCheck(base, name);
  const auto found = known.find(texture);
  if (found != known.end() && found->second.name == name && found->second.check == check) return found->second.lightmap;
  const bool lightmap = LightmapTextureByName(base, name);
  if (known.size() > 65536) known.clear();
  known[texture] = {name, check, lightmap};
  return lightmap;
}

bool LightmapTextureByName(uint8_t* base, uint32_t name) {
  const std::string lower = Lower(InlineName(base, name + 4));
  const size_t size = lower.size();
  if (lower.find("detail") != std::string::npos) return false;
  for (const char* end : {"_n", "_c", "_s", "_d"}) {
    if (size > 2 && lower.compare(size - 2, 2, end) == 0) return false;
  }
  return true;
}

// side208: what a sampler parameter's texture variable name says, worked out once per parameter instead of for every
// draw every frame (the names are the level's material data; the main thread's profile, side199, had the per-draw
// name reading and lowercasing in ngl_side's 28-37%). An entry is checked against the text's first words, so a
// parameter address used again by the next level is worked out again.
enum NameKind : uint16_t {  // [rexglue-native] rn32: 16 bits (was 8, all taken)
  kNameEnv = 1,
  kNameSpecular = 2,
  kNameNormal = 4,
  kNameSecondColor = 8,
  kNameColor = 16,
  kNameOther = 32,
  kNameFabricMask = 64,     // [rexglue-native] rn24
  kNameNormalSwatch = 128,  // [rexglue-native] rn24
  kNameNoiseSwatch = 256,   // [rexglue-native] rn32
  kNameScratch = 512,       // [rexglue-native] rn34
  kNameDetail = 1024,       // [rexglue-native] rn36
  kNameReflectionCube = 2048,  // [rexglue-native] rn40e
};

// A fingerprint of the 32 bytes from text's word on (Readable wants a word's address).
uint32_t TextCheck(uint8_t* base, uint32_t text) {
  uint32_t check = text;
  for (uint32_t at = text & ~3u, k = 0; k < 32; k += 4) {
    if (!Readable(at + k)) break;
    check = check * 0x9E3779B1u ^ g::ReadU32(base, at + k);
  }
  return check;
}

uint16_t ParameterKinds(uint8_t* base, uint32_t parameter) {
  struct Entry {
    uint32_t check;
    uint16_t kinds;
  };
  static std::unordered_map<uint32_t, Entry> kinds_of;
  const uint32_t text = Readable(parameter) ? g::ReadU32(base, parameter + kParameterSamplerText) : 0;
  const uint32_t check = Pointer(text) && Readable(text) ? TextCheck(base, text) : 0;
  const auto found = kinds_of.find(parameter);
  if (found != kinds_of.end() && found->second.check == check) return found->second.kinds;
  const std::string name = SamplerTextureName(base, parameter);
  const uint16_t kinds = (EnvMapName(name) ? kNameEnv : 0) | (SpecularMapName(name) ? kNameSpecular : 0) |
                         (NormalMapName(name) ? kNameNormal : 0) | (SecondColorMapName(name) ? kNameSecondColor : 0) |
                         (ColorMapName(name) ? kNameColor : 0) | (OtherMapName(name) ? kNameOther : 0) |
                         (FabricMaskName(name) ? kNameFabricMask : 0) |      // [rexglue-native] rn24
                         (NormalSwatchName(name) ? kNameNormalSwatch : 0) |  // [rexglue-native] rn24
                         (NoiseSwatchName(name) ? kNameNoiseSwatch : 0) |    // [rexglue-native] rn32
                         (ScratchName(name) ? kNameScratch : 0) |            // [rexglue-native] rn34
                         (DetailName(name) ? kNameDetail : 0) |              // [rexglue-native] rn36
                         (ReflectionCubeName(name) ? kNameReflectionCube : 0);  // [rexglue-native] rn40e
  if (kinds_of.size() > 65536) kinds_of.clear();
  kinds_of[parameter] = {check, kinds};
  return kinds;
}

// The color map is the texture whose sampler names it so (side33; a set's look by the surface's frame, side49 and
// side86), else the first DXT1 or DXT3 texture that isn't the lightmap (the DXT5 ones seen are normal maps), else the
// first texture.
Samplers MaterialSamplers(uint8_t* base, uint32_t material, bool skinned, uint32_t frame = 0) {
  Samplers out;
  const uint32_t list = Pointer(material) ? g::ReadU32(base, material + kMaterialSamplers) : 0;
  if (!Readable(list)) return out;
  for (uint32_t k = 0; k < kMaxSamplers; ++k) {  // by name first
    const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
    const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
    if (!Pointer(parameter)) break;
    if (parameter == kLightMapParameter) {
      const bool lightmap = skinned ? IsTexture(base, texture) && Physical(texture) : LightmapTexture(base, texture);
      if (lightmap) out.lightmap = texture;
      continue;
    }
    const uint16_t kinds = ParameterKinds(base, parameter);  // side208
    if (IsTexture(base, texture)) {
      if (!out.env && (kinds & kNameEnv)) {
        out.env = texture;
        continue;
      }
      if (!out.scratch && (kinds & kNameScratch)) {  // [rexglue-native] rn34: not the specular map, though it says spec
        out.scratch = texture;
        continue;
      }
      if (!out.env_mask && (kinds & kNameSpecular)) out.env_mask = texture;
      if (!out.normal && (kinds & kNameNormal)) out.normal = texture;
      if (!out.fabric_mask && (kinds & kNameFabricMask)) out.fabric_mask = texture;          // [rexglue-native] rn24
      if (!out.normal_swatch && (kinds & kNameNormalSwatch)) out.normal_swatch = texture;    // [rexglue-native] rn24
      if (!out.noise_swatch && (kinds & kNameNoiseSwatch)) out.noise_swatch = texture;       // [rexglue-native] rn32
      if (!out.detail && (kinds & kNameDetail)) out.detail = texture;                         // [rexglue-native] rn36
      if (!out.reflection_cube && (kinds & kNameReflectionCube)) {  // [rexglue-native] rn40e: never the color map
        out.reflection_cube = texture;
        continue;
      }
      if (kinds & kNameSecondColor) {  // side195
        if (!out.color_b) out.color_b = texture;
        continue;
      }
    }
    if (out.color || !(kinds & kNameColor)) continue;
    if (IsTexture(base, texture)) {
      out.color = texture;
    } else if ((out.color = LookFor(base, texture, frame)) != 0) {
      out.looks = texture;
    }
  }
  if (out.color) return out;
  out.lightmap = 0;
  uint32_t first = 0;
  for (uint32_t k = 0; k < kMaxSamplers; ++k) {
    const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
    const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
    if (!Pointer(parameter)) break;
    if (!IsTexture(base, texture)) continue;
    if (parameter == kLightMapParameter) {
      // one in the image is a global (a default or a render target); side156: a detail normal map isn't one (a
      // soldier's color map under it is kept as before, for the check below)
      if (skinned ? Physical(texture) : LightmapTexture(base, texture)) out.lightmap = texture;
      continue;
    }
    if (ParameterKinds(base, parameter) & kNameOther) continue;  // side208
    if (!first) first = texture;
    const uint32_t format = g::ReadU32(base, entry + 8 + 4) & 0x3F;  // the fetch constant's dword 1
    if (!out.color && (format == 18 || format == 19)) out.color = texture;
  }
  if (!out.color) out.color = first;
  // A soldier's list (side24) has its own DXT5 texture under 0x829114E0 and again as the next entry, then shared
  // ones: there the 0x829114E0 texture is the color map, not a lightmap. (The terrain's lists also name their
  // lightmap twice, so this is for skinned surfaces only; side25 turned the whole ground into its lightmap.)
  if (skinned && out.lightmap) {
    for (uint32_t k = 0; k < kMaxSamplers; ++k) {
      const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
      const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
      if (!Pointer(parameter)) break;
      if (parameter != kLightMapParameter && texture == out.lightmap) {
        out.color = out.lightmap;
        out.lightmap = 0;
        out.looks = 0;
        break;
      }
    }
  }
  return out;
}

// The first materials' sampler lists in full, every texture saved (ngl-side-texture-<address>.ppm), to learn which
// entry is which map: "material M (stride S): 0 T WxH format F, 1 ..." (SaveTexture is below).
void SaveTexture(uint8_t* base, uint32_t texture);

// A name for an effect parameter, if one of its first eight words points to a short printable string ("-" if not).
std::string ParameterName(uint8_t* base, uint32_t parameter) {
  if (!Readable(parameter)) return "-";
  for (uint32_t at = 0; at < 0x20; at += 4) {
    const uint32_t text = g::ReadU32(base, parameter + at);
    if (!Readable(text & ~3u) || text < 0x82000000u) continue;
    std::string name;
    for (uint32_t k = 0; k < 40; ++k) {
      const uint8_t c = base[text + k];
      if (c == 0) break;
      if (c < 0x20 || c > 0x7E) {
        name.clear();
        break;
      }
      name += static_cast<char>(c);
    }
    if (name.size() >= 3) return "\"" + name + "\" at +" + std::to_string(at);
  }
  return "-";
}

std::string DeepName(uint8_t* base, uint32_t at);

// side31: the stride-52 lists shown were terrain pieces, not soldiers, so the skinned ones are picked by their vertices
// now: past the first 24 materials, 8 more skinned ones.
void ShowSamplers(uint8_t* base, uint32_t material, uint32_t stride, bool skinned, bool always = false) {
  static std::unordered_set<uint32_t> shown;
  static int skinned_shown = 0;
  if (!Pointer(material) || (shown.count(material) && !always)) return;
  if (!always && shown.size() >= 24 && !(skinned && skinned_shown++ < 8)) return;
  shown.insert(material);
  const uint32_t list = g::ReadU32(base, material + kMaterialSamplers);
  if (!Readable(list)) return;
  std::string text;
  for (uint32_t k = 0; k < kMaxSamplers; ++k) {
    const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
    const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
    if (!Pointer(parameter)) break;
    char item[320];
    const std::string texture_name = SamplerTextureName(base, parameter);
    std::string name = texture_name.empty() ? DeepName(base, parameter) : "<" + texture_name + ">";
    // side32: a soldier's parameter holds the sampler itself (+0 its texture, then the fetch constant), with +0x20 and
    // +0x28 pointers and +0x24 a small number (6): the name is looked for behind those, and the number shown
    for (uint32_t at : {0x20u, 0x28u}) {
      const uint32_t inner = Readable(parameter) ? g::ReadU32(base, parameter + at) : 0;
      if (name == "-" && Pointer(inner) && Readable(inner)) {
        name = DeepName(base, inner);
        if (name != "-") name = "+" + std::to_string(at) + " -> " + name;
      }
    }
    if (Readable(parameter)) name += " #" + std::to_string(g::ReadU32(base, parameter + 0x24));
    if (IsTexture(base, texture)) {
      std::snprintf(item, sizeof item, "%s%u %08X %ux%u format %u (parameter %08X %s)", text.empty() ? "" : ", ", k,
                    texture, g::ReadU32(base, texture + kTextureWidth), g::ReadU32(base, texture + kTextureHeight),
                    g::ReadU32(base, entry + 12) & 0x3F, parameter, name.c_str());
      if (Physical(texture)) SaveTexture(base, texture);
    } else {
      std::snprintf(item, sizeof item, "%s%u (%08X, not a texture; parameter %08X %s)", text.empty() ? "" : ", ", k,
                    texture, parameter, name.c_str());
    }
    text += item;
  }
  REXLOG_INFO("cod3: ngl side: material {:08X} (stride {}{}): {}", material, stride, skinned ? ", skinned" : "",
              text);
  if (skinned && Readable(g::ReadU32(base, list + kSamplerFirst))) {  // a soldier's first parameter in full
    const uint32_t parameter = g::ReadU32(base, list + kSamplerFirst);
    REXLOG_INFO("cod3: ngl side: material {:08X} parameter {:08X}: {} / {}", material, parameter,
                Words(base, parameter, 8), Words(base, parameter + 0x20, 8));
    for (uint32_t at : {0x20u, 0x28u}) {  // and what its two pointers point at
      const uint32_t inner = g::ReadU32(base, parameter + at);
      if (Pointer(inner) && Readable(inner))
        REXLOG_INFO("cod3: ngl side: material {:08X} parameter {:08X} +{:02X} -> {:08X}: {} / {}", material, parameter,
                    at, inner, Words(base, inner, 8), Words(base, inner + 0x20, 8));
    }
  }
}

// A shader state's parameter list (node +0x18, the list at +0x34), for the first states seen: each parameter's
// semantic code, a name if one is found, its first words, and the four floats where its value goes (0x82142F20 fills
// them on each pass, so these are the game's last), to find FogColor and the lights' values:
// "state S parameter K: code C, value at V (a b c d), name N, words ...".
constexpr uint32_t kNodeShaderState = 0x18;
constexpr uint32_t kStateParameters = 0x34;
constexpr uint32_t kParameterNext = 0x0C;
constexpr uint32_t kParameterCode = 0x10;

// [rexglue-native] 2026-11-09 rn30: which of the game's two uniform shaders draws a fabric pass, from that pass's own
// binding (LogPassBinding's tables; rn28): the view model's sleeves (3BA30872B3955C23) take the irradiance rows (the
// light block's +0xF0) from c18; the soldiers' own uniforms (21168F53BCF9D156) from c19, with c30 COD_SHADOW_FARZ and
// c31 COD_SHADOW_GREYLEVEL from the state's value block (slot 1: the state's +0x1C). Read for each draw, as the game
// reads them; when the pass can't be read the draw stays the view model's kind.
// rn31: and the sun as the pass binds it: __1_COD_SHADOW_PROJCOLOR_cmn (the state's +0x6B0: 3BA30872's c27,
// 21168F53's c29) and position_Light0 (+0x5F0: 21168F53's c28, the place the sun is taken from, about 2000 units from
// the soldier). rn30's log: the native sun of the near soldier's uniform (diffuse B4190000) was its light set's
// (0.373 0.358 0.309, the scene's sun / 2.4 x the light grid's share) where the game's c29 was 0.905 0.868 0.749.
// rn32: the soldiers' skin (the heads: 3949D29E09A78AA1, rows from c20) is told apart the same way. Each value is
// read from the register its shader reads it from, as the pass binds that register: the value block's layout is the
// state's own (rn32's skin pass binds c33 position_Light0 +0x5D0, c34 PROJCOLOR +0x690, c35 COD_SHADOW_FARZ +0xA40,
// c36 COD_SHADOW_GREYLEVEL +0xB00, each 0x20 below the uniforms' state's).
// where the irradiance rows (the light block's +0xF0) start: c18 the view model's sleeves (3BA30872B3955C23), c19 the
// soldiers' uniforms (21168F53BCF9D156, and ADC02DCE4FDC9253), c20 their skin
constexpr uint32_t kRowsViewModelFabric = 18;
constexpr uint32_t kRowsSoldierFabric = 19;
constexpr uint32_t kRowsSoldierSkin = 20;
// the registers each of those shaders reads the sun's place and color, COD_SHADOW_FARZ and COD_SHADOW_GREYLEVEL from
// (from their microcode, rn9/dump; 0: it has none)
struct SunRegisters {
  uint32_t rows, place, color, farz, grey;
};
constexpr SunRegisters kSunRegisters[] = {
    {kRowsViewModelFabric, 0, 27, 0, 0},  // 3BA30872B3955C23
    {kRowsSoldierFabric, 28, 29, 30, 31},  // 21168F53BCF9D156
    {kRowsSoldierSkin, 33, 34, 35, 36},    // 3949D29E09A78AA1
};
struct FabricPass {
  bool third_person = false;  // the soldiers' uniforms (rows from c19)
  bool skin = false;          // rn32: the soldiers' skin (rows from c20)
  float shadow_fade = 0.0f;   // COD_SHADOW_FARZ's x
  float shadow_floor = 0.0f;  // COD_SHADOW_GREYLEVEL's x
  bool sun_bound = false;     // rn31: the pass binds PROJCOLOR (as read: the caller checks it)
  float sun_color[3] = {};
  bool sun_placed = false;    // and position_Light0
  float sun_place[3] = {};
};
// rn34: a pass's pixel shader constants as the pass binds them (LogPassBinding's table: node +0x1C technique, +0x0C its
// first pass, pass +0xDC compiled, the pixel shader's float constants at +0xC0), register by register: the slot (0:
// none) and offset each is filled from, the register the irradiance rows (the light block's +0xF0) start at, and the
// state's value block (slot 1: the state's +0x1C; 0 when it can't be read). ok: the table was read.
struct PassBinding {
  static constexpr uint32_t kMaxRegisters = 64;
  uint32_t slot[kMaxRegisters] = {}, offset[kMaxRegisters] = {};
  uint32_t rows_at = 0;
  uint32_t values = 0;
  bool ok = false;
};
PassBinding ReadPassBinding(uint8_t* base, uint32_t node) {
  PassBinding out;
  const uint32_t technique = GrassReadable(node) ? g::ReadU32(base, node + 0x1C) : 0;
  const uint32_t pass = GrassReadable(technique) ? g::ReadU32(base, technique + 0x0C) : 0;
  const uint32_t compiled = GrassReadable(pass) ? g::ReadU32(base, pass + 0xDC) : 0;
  const uint32_t state = GrassReadable(node) ? g::ReadU32(base, node + kNodeShaderState) : 0;
  if (!GrassReadable(compiled)) return out;
  const uint32_t at = compiled + 0xC0;  // the pixel shader's float constants (LogPassBinding)
  const uint32_t count = g::ReadU32(base, at + 0x08), rows = g::ReadU32(base, at + 0x20);
  const uint32_t first = (g::ReadU32(base, at + 0x1C) & 0x7FF) / 4;  // from 256: the pixel shader's c0
  const uint32_t values = GrassReadable(state) ? g::ReadU32(base, state + 0x1C) : 0;
  for (uint32_t k = 0; k < count && k < 32 && GrassReadable(rows + 8 * k); ++k) {
    const uint32_t slot = g::ReadU32(base, rows + 8 * k) / 4, offset = g::ReadU32(base, rows + 8 * k + 4);
    const uint32_t constant = first + k - 256;
    if (constant >= PassBinding::kMaxRegisters) continue;
    out.slot[constant] = slot;
    out.offset[constant] = offset;
    if (slot == 4 && offset == 0xF0) out.rows_at = constant;
  }
  out.values = GrassReadable(values) ? values : 0;
  out.ok = true;
  return out;
}
// rn43a: the value at a value block's offset as the game drew the object being built (cod3_debug_ngl_side_drawn_values;
// false when none was kept: then read the block as it is now)
bool DrawnValue(uint8_t* base, uint32_t block, uint32_t offset, float* into, uint32_t floats);
// a register bound to the state's value block (slot 1), read as up to four floats; false when it isn't (or a value
// isn't finite). rn43a: as the game drew the object, when that was kept (DrawnValue).
bool ReadBound(uint8_t* base, const PassBinding& binding, uint32_t constant, float* into, uint32_t floats) {
  if (!binding.values || !constant || constant >= PassBinding::kMaxRegisters || binding.slot[constant] != 1) {
    return false;
  }
  const uint32_t from = binding.values + binding.offset[constant];
  if (!GrassReadable(from) || !GrassReadable(from + 4 * (floats - 1))) return false;
  if (floats <= 4 && DrawnValue(base, binding.values, binding.offset[constant], into, floats)) return true;
  bool finite = true;
  for (uint32_t i = 0; i < floats; ++i) {
    into[i] = g::ReadF32(base, from + 4 * i);
    finite = finite && std::isfinite(into[i]);
  }
  return finite;
}

// [rexglue-native] 2026-11-10 rn41q: the effect a node's first pass was compiled from, by the pass's own text
// (rn41n-p: a pass's +0xD8 points to {its annotations, a count, its state text}, and the text reads "Draw=Geometry;"
// then "... PixelShader = compile ps_3_0 standard_7_1_PixelShader(); ..."): "standard_7_1", or "" when there is
// no such text. Read only through Readable and the GPU view (rn41m), and kept per pass (a pass's text doesn't change).
std::string PassEffect(uint8_t* base, uint32_t node) {
  static std::mutex mutex;
  static std::unordered_map<uint32_t, std::string> kept;
  auto safe = [](uint32_t a, uint32_t span) { return Readable(a) && Readable(a + span); };
  const uint32_t technique = GrassReadable(node) ? g::ReadU32(base, node + 0x1C) : 0;
  const uint32_t pass = safe(technique, 0x10) ? GpuU32(base, technique + 0x0C) : 0;
  if (!safe(pass, 0xE0)) return {};
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = kept.find(pass);
    if (it != kept.end()) return it->second;
  }
  constexpr uint32_t kTextBytes = 0x200;
  std::string name;
  const uint32_t block = GpuU32(base, pass + 0xD8);
  const uint32_t text = safe(block, 0x0C) ? GpuU32(base, block + 8) : 0;
  if (safe(text, kTextBytes)) {
    std::string chars;
    for (uint32_t i = 0; i < kTextBytes; ++i) {  // the words' bytes, high first
      chars += static_cast<char>((GpuU32(base, text + (i & ~3u)) >> (8 * (3 - (i & 3)))) & 0xFF);
    }
    const std::string key = "PixelShader = compile ps_3_0 ", tail = "_PixelShader";
    const size_t at = chars.find(key);
    if (chars.compare(0, 5, "Draw=") == 0 && at != std::string::npos) {
      size_t end = at + key.size();
      while (end < chars.size() && (std::isalnum(static_cast<unsigned char>(chars[end])) || chars[end] == '_')) ++end;
      const std::string function = chars.substr(at + key.size(), end - at - key.size());
      if (function.size() > tail.size() && function.compare(function.size() - tail.size(), tail.size(), tail) == 0) {
        name = function.substr(0, function.size() - tail.size());
      }
    }
  }
  std::lock_guard<std::mutex> lock(mutex);
  if (kept.size() < 8192) kept.emplace(pass, name);
  return name;
}

FabricPass ReadFabricPass(uint8_t* base, uint32_t node) {
  FabricPass out;
  const PassBinding binding = ReadPassBinding(base, node);  // rn34: (was read here)
  if (!binding.ok) return out;
  out.third_person = binding.rows_at == kRowsSoldierFabric;
  out.skin = binding.rows_at == kRowsSoldierSkin;  // rn32
  const SunRegisters* registers = nullptr;
  for (const SunRegisters& r : kSunRegisters) {
    if (r.rows == binding.rows_at) registers = &r;
  }
  if (!registers || !binding.values) return out;
  out.sun_bound = ReadBound(base, binding, registers->color, out.sun_color, 3);   // rn31
  out.sun_placed = ReadBound(base, binding, registers->place, out.sun_place, 3);
  float fade = 0.0f, floor = 0.0f;
  if ((out.third_person || out.skin) && ReadBound(base, binding, registers->farz, &fade, 1) &&
      ReadBound(base, binding, registers->grey, &floor, 1)) {
    out.shadow_fade = fade;
    out.shadow_floor = floor;
  }
  return out;
}

// [rexglue-native] 2026-11-09 rn34: the guns' metal pass (pixel shader CFC45995D1294CCE: the Springfield, the BREN;
// rn21/rn22). Its irradiance rows start at c19, as the soldiers' uniforms' do, but it also binds c32: its material, all
// from the state's value block, is texture_brightness c29.x, Reflection_Amt c30.x, SpecularPower c31.x and Metal_Hue
// c32.xyz (The Mace's Springfield: 1, 0.25, 35, (1.122 1.049 1.047)); COD_SHADOW_PROJCOLOR is its c28, and c17.x a bias
// the shader adds to the cube's direction (0 there). ok only when all four of the material's are bound and read.
constexpr uint32_t kRowsMetal = 19;
struct MetalPass {
  bool ok = false;
  float brightness = 1.0f, reflection = 0.0f, power = 1.0f;
  float hue[3] = {1.0f, 1.0f, 1.0f};
  float cube_bias = 0.0f;
  bool sun_bound = false;  // c28 read from the value block (else the light block's, as before)
  float sun_color[3] = {};
};
MetalPass ReadMetalPass(uint8_t* base, uint32_t node) {
  MetalPass out;
  const PassBinding binding = ReadPassBinding(base, node);
  if (!binding.ok || binding.rows_at != kRowsMetal || !binding.values) return out;
  float brightness = 0.0f, reflection = 0.0f, power = 0.0f, hue[3] = {};
  if (!ReadBound(base, binding, 29, &brightness, 1) || !ReadBound(base, binding, 30, &reflection, 1) ||
      !ReadBound(base, binding, 31, &power, 1) || !ReadBound(base, binding, 32, hue, 3)) {
    return out;
  }
  out.brightness = brightness;
  out.reflection = reflection;
  out.power = power;
  std::memcpy(out.hue, hue, sizeof hue);
  out.sun_bound = ReadBound(base, binding, 28, out.sun_color, 3);
  float bias = 0.0f;
  if (ReadBound(base, binding, 17, &bias, 1)) out.cube_bias = bias;
  out.ok = true;
  return out;
}

// [rexglue-native] 2026-11-09 rn36: the vehicles' detail-mapped metal (The Mace's tank body: "sherman_firefly_c" with
// "v_metal_detail01_n" as its Detail_Texture; rn35b's host-draw log). The game draws it with C90DF51FF5AAF4F8 in the
// sun's shadow range and with 8E7F713B9E3C1FAD beyond it (rn9/dump), both taking the irradiance rows from c18 and the
// rest from the state's value block (slot 1):
//   C90DF51F: c27 position_Light0, c28 __1_COD_SHADOW_PROJCOLOR_cmn, c29 COD_SHADOW_FARZ, c30 COD_SHADOW_GREYLEVEL,
//             c31 the detail's repeat, c32 its z scale, c33 the normal map's z scale, c34 the highlights' power;
//   8E7F713B: c27 the detail's repeat, c28 its z scale, c29 the normal map's z scale, c30 the power, c31 the sun's place
//             (10^6 units off), c32 its color; no shadow, floor or fade.
// rn35b's values: (847 -5506 980), (0.905 0.868 0.749), 0.001, 0.4, 15.2, 1, 1, 30; beyond it: 15.2, 1, 1, 30,
// (852868 -150384 500000), (1.320 1.222 0.983). The two are told apart by c31: the sun's place is far off (over 10^4
// units), the repeat isn't. ok when every one of them is bound to the value block and read.
// rn37: a scalar's other three lanes in the value block hold the heap's fill (0xCDCDCDCD, -4.3e8 as a float; rn35b's
// GPU c31 of C90DF51F: (15.2 -4.316e8 -4.316e8 -4.316e8)), so rn36 took that c31 for a sun's place 4.3e8 units off and
// read the shadowed pass as 8E7F713B's (repeat 847.5, power 0.4). A place now needs all three parts written, and
// every value read is checked for its range: a sun's color 0 to 64 (as StateSun's), the z scales 0 to 16, the repeat
// and the power 0 to 1000; when one is out of it the draw stays bumped as before.
constexpr uint32_t kRowsDetail = 18;
constexpr float kSunPlaceFar = 1e4f;
constexpr uint32_t kValueFill = 0xCDCDCDCD;  // as StateSun's kUnwrittenFill (rn18)
bool FillValue(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof bits);
  return bits == kValueFill;
}
struct DetailPass {
  bool ok = false;
  bool shadowed = false;  // C90DF51F (in the sun's shadow range); else 8E7F713B
  float repeat = 1.0f, detail_scale = 1.0f, normal_scale = 1.0f, power = 30.0f;
  float sun_color[3] = {}, sun_place[3] = {};
  float shadow_fade = 0.0f, shadow_floor = 0.0f;
};
DetailPass ReadDetailPass(uint8_t* base, uint32_t node) {
  DetailPass out;
  const PassBinding binding = ReadPassBinding(base, node);
  if (!binding.ok || binding.rows_at != kRowsDetail || !binding.values) return out;
  float c31[3] = {};
  if (!ReadBound(base, binding, 31, c31, 3)) return out;
  const bool beyond = !FillValue(c31[0]) && !FillValue(c31[1]) && !FillValue(c31[2]) &&
                      std::sqrt(c31[0] * c31[0] + c31[1] * c31[1] + c31[2] * c31[2]) > kSunPlaceFar;  // rn37
  float repeat = 0.0f, detail_scale = 0.0f, normal_scale = 0.0f, power = 0.0f;
  if (beyond) {
    if (!ReadBound(base, binding, 27, &repeat, 1) || !ReadBound(base, binding, 28, &detail_scale, 1) ||
        !ReadBound(base, binding, 29, &normal_scale, 1) || !ReadBound(base, binding, 30, &power, 1) ||
        !ReadBound(base, binding, 32, out.sun_color, 3)) {
      return out;
    }
    std::memcpy(out.sun_place, c31, sizeof c31);
  } else {
    float fade = 0.0f, floor = 0.0f;
    if (!ReadBound(base, binding, 27, out.sun_place, 3) || !ReadBound(base, binding, 28, out.sun_color, 3) ||
        !ReadBound(base, binding, 29, &fade, 1) || !ReadBound(base, binding, 30, &floor, 1) ||
        !ReadBound(base, binding, 32, &detail_scale, 1) || !ReadBound(base, binding, 33, &normal_scale, 1) ||
        !ReadBound(base, binding, 34, &power, 1)) {
      return out;
    }
    repeat = c31[0];
    out.shadowed = true;
    out.shadow_fade = fade;
    out.shadow_floor = floor;
  }
  if (!(repeat > 0.0f && repeat < 1000.0f && power > 0.0f && power < 1000.0f)) return out;
  // rn37: the rest in range too (and none of it the fill)
  constexpr float kMostScale = 16.0f, kMostColor = 64.0f;
  if (!(detail_scale >= 0.0f && detail_scale <= kMostScale && normal_scale >= 0.0f && normal_scale <= kMostScale)) {
    return out;
  }
  for (int i = 0; i < 3; ++i) {
    if (!(out.sun_color[i] >= 0.0f && out.sun_color[i] <= kMostColor) || FillValue(out.sun_place[i])) return out;
  }
  if (out.shadowed && !(out.shadow_fade >= 0.0f && out.shadow_fade < 1.0f && out.shadow_floor >= 0.0f &&
                        out.shadow_floor <= 1.0f)) {
    return out;
  }
  out.repeat = repeat;
  out.detail_scale = detail_scale;
  out.normal_scale = normal_scale;
  out.power = power;
  out.ok = true;
  return out;
}

// [rexglue-native] 2026-11-10 rn39: the vehicles' bumped parts with a specular map (The Mace's tank treads
// "sherman_tread_c" and wheels "sherman_wheel1_c" / "sherman_wheel2_c"; rn35b's host-draw log). The game draws them
// with five pixel shaders, all reading the nine irradiance rows (the light block's +0xF0) and the rest from the state's
// value block (slot 1), all from the shaders' microcode (work folder acct2-bro-shaders/shader_<hash>.ucode.frag) or,
// for the last, its pass's binding (rn39c's log):
//   - B2E0DA0062DFF88D (treads; vertex shader FD1BB8BC812AA146): rows from c18; c16 / c17 the nearest point light's
//     place and color, c27 position_Light0, c28 __1_COD_SHADOW_PROJCOLOR_cmn, c29 COD_SHADOW_FARZ, c30
//     COD_SHADOW_GREYLEVEL, c31.x the normal map's scale, c32.x the highlights' power. The shadow: 3 x 3 point reads
//     of the sun's depth map weighted by the quadratic B-spline of the read's fraction (instructions 12-22, 35-55,
//     63; c254 (0.5 -1 0 1)), s = how much of the sun reaches the pixel; the sun = max(mix(s x c28, 1,
//     saturate(the view depth x c29.x)), c30.x) (61, 65, 68-69, 73).
//   - 9A3D3175A6CAFE56 (the near tank's treads; vertex shader 38632A02D1F31F15; the pass the native side reads for
//     them, rn39's log): rows from c20; c16 the eye's place, c17 / c18.x / c19 the nearest point light's place,
//     1 / range and color, c29-c32 the shadow map's matrix (the shadow's place taken per pixel), c33 position_Light0,
//     c34 PROJCOLOR, c35.x the normal map's scale, c36.x the power. The shadow: 2 x 2 point reads, bilinear weights
//     (21-24, 30, 57-59); the sun = s x c34, no floor or fade (69). Its color map is alpha-tested and read at the
//     coordinates plus a second set (the treads' scroll, 8): not drawn natively yet.
//   - 7D4E7CAD9503762D (the wheels; vertex shader 7DBF6C7381AFF923): rows from c18; c16 / c17 the nearest point light,
//     c27 position_Light0, c28 PROJCOLOR, c29.x the normal map's scale, c30.x the power. The shadow as 9A3D3175's
//     (23-26, 31, 48, 55); the sun = s x c28, no floor or fade (65). Its color map is alpha-tested and turned about
//     its middle by the wheel's angle (8-15): not drawn natively yet.
//   - beyond the sun's shadow range (rn39c: at The Mace's frame 2900 the sun's place in the value block was 852868
//     -150384 500000, 10^6 units off, a sun from a fixed way), the wheels' and treads' passes are bound without a
//     shadow: BA59B7419D390DB8 (the wheels; its color map turned and alpha-tested as 7D4E7CAD's) and
//     E470EE60FDF3D47D, rows from c18, c27.x the normal map's scale, c28.x the power, c29 position_Light0, c30
//     PROJCOLOR (their instructions 11 / 26, 20 / 35, 39 / 63, 43 / 67), no shadow, floor or fade; and the treads'
//     unshadowed pass (compiled at 8272C800, rows from c20 and the eye at c16 as 9A3D3175's): c29 and c30 from the
//     value block's +FB0 and +1140 (9A3D3175's c35 and c36 there, the scale and the power), c31 and c32 from +1700
//     and +17C0 (the far sun's place and color).
// In all of them: the normal map's x and y times the scale, z = sqrt(|1 - x^2 - y^2|) of the unscaled ones; the light =
// the nine rows + N.L x the sun + N.L x the point light (its color x its falloff); the highlights = (the sun x (N.H)^p
// + the point light x (N.H')^p) x the specular map, H toward the eye's own place; out = the light x the color map +
// the highlights (no doubling, nothing held to 1).
// They are told apart by the pass's binding (VehicleLayout): rows from c20 with c33-c36 bound (9A3D3175; the soldiers'
// skin 3949D29E09A78AA1 binds the same registers, but its materials have a Noise_Swatch and are taken before this) or
// with c29-c32 bound and not c33 (the treads' unshadowed pass); rows from c18 with c31 and c32 bound (B2E0DA00), or
// with c27-c30 bound and not c31, where c27 is a place (7D4E7CAD) or a scalar whose other three lanes hold the heap's
// fill (BA59B741 / E470EE60; rn37's test). The detail-mapped metal (C90DF51F, also from c18) is taken before this. ok
// when every register is bound to the value block, read and in its range (as ReadDetailPass's, and a power of 1 or
// more). (rn39b also wanted the sun's place within 10^5 units of the object, which turned down every one of them
// whenever the game's sun was the far one: rn39c.)
struct VehicleRegisters {
  const char* shader;
  uint32_t place, color;   // position_Light0, PROJCOLOR
  uint32_t fade, floor;    // COD_SHADOW_FARZ, COD_SHADOW_GREYLEVEL (0: none)
  uint32_t scale, power;   // the normal map's scale, the highlights' power
};
constexpr VehicleRegisters kVehicleTreadsB2E0 = {"B2E0DA0062DFF88D", 27, 28, 29, 30, 31, 32};
constexpr VehicleRegisters kVehicleWheels7D4E = {"7D4E7CAD9503762D", 27, 28, 0, 0, 29, 30};
constexpr VehicleRegisters kVehicleBeyondBA59 = {"BA59B7419D390DB8 / E470EE60FDF3D47D", 29, 30, 0, 0, 27, 28};
constexpr VehicleRegisters kVehicleTreads9A3D = {"9A3D3175A6CAFE56", 33, 34, 0, 0, 35, 36};
constexpr VehicleRegisters kVehicleTreadsBeyond = {"the treads' unshadowed pass (rows from c20)", 31, 32, 0, 0, 29, 30};
constexpr uint32_t kRowsVehicleNear = 20;  // 9A3D3175's and the treads' unshadowed pass's
const VehicleRegisters* VehicleLayout(uint8_t* base, const PassBinding& binding) {
  const auto bound = [&](uint32_t first, uint32_t last) {
    for (uint32_t c = first; c <= last; ++c) {
      if (binding.slot[c] != 1) return false;
    }
    return true;
  };
  if (binding.rows_at == kRowsDetail) {
    if (bound(31, 32)) return &kVehicleTreadsB2E0;
    if (!bound(27, 30) || binding.slot[31] == 1) return nullptr;
    float c27[3] = {};
    if (!ReadBound(base, binding, 27, c27, 3)) return nullptr;
    return FillValue(c27[1]) && FillValue(c27[2]) ? &kVehicleBeyondBA59 : &kVehicleWheels7D4E;
  }
  if (binding.rows_at == kRowsVehicleNear) {
    if (bound(33, 36)) return &kVehicleTreads9A3D;
    if (bound(29, 32) && binding.slot[33] != 1) return &kVehicleTreadsBeyond;
  }
  return nullptr;
}
// (rn39c: why is set whenever ok is false, and the numbers as far as they were read, for the debug log)
struct VehiclePass {
  bool ok = false;
  const char* shader = "";
  const char* why = "";
  uint32_t rows_at = 0;
  float away = 0.0f;  // the sun's place from the object
  float normal_scale = 1.0f, power = 30.0f;
  float sun_color[3] = {}, sun_place[3] = {};
  float shadow_fade = 0.0f, shadow_floor = 0.0f;
};
VehiclePass ReadVehiclePass(uint8_t* base, uint32_t node, uint32_t object) {
  VehiclePass out;
  const PassBinding binding = ReadPassBinding(base, node);
  out.rows_at = binding.rows_at;
  if (!binding.ok || !binding.values || !Readable(object + 0x38)) {
    out.why = "the pass's binding or values can't be read";
    return out;
  }
  const VehicleRegisters* registers = VehicleLayout(base, binding);
  if (!registers) {
    out.why = "not bound as any of the five";
    return out;
  }
  out.shader = registers->shader;
  float place[3] = {}, color[3] = {}, fade = 0.0f, floor = 0.0f, normal_scale = 0.0f, power = 0.0f;
  if (!ReadBound(base, binding, registers->place, place, 3) || !ReadBound(base, binding, registers->color, color, 3) ||
      !ReadBound(base, binding, registers->scale, &normal_scale, 1) ||
      !ReadBound(base, binding, registers->power, &power, 1)) {
    out.why = "a register isn't bound to the value block, or isn't a number";
    return out;
  }
  if (registers->fade && (!ReadBound(base, binding, registers->fade, &fade, 1) ||
                          !ReadBound(base, binding, registers->floor, &floor, 1))) {
    out.why = "the fade or the floor isn't bound to the value block, or isn't a number";
    return out;
  }
  std::memcpy(out.sun_place, place, sizeof place);
  std::memcpy(out.sun_color, color, sizeof color);
  out.shadow_fade = fade;
  out.shadow_floor = floor;
  out.normal_scale = normal_scale;
  out.power = power;
  constexpr float kMostScale = 16.0f, kMostColor = 64.0f;
  float away = 0.0f;
  for (int i = 0; i < 3; ++i) {
    if (FillValue(place[i]) || !(color[i] >= 0.0f && color[i] <= kMostColor)) {
      out.why = "the sun's place is the heap's fill, or its color is out of range";
      return out;
    }
    const float part = place[i] - g::ReadF32(base, object + 0x30 + 4 * i);
    away += part * part;
  }
  out.away = std::sqrt(away);
  if (!std::isfinite(out.away) || !(fade >= 0.0f && fade < 1.0f) || !(floor >= 0.0f && floor <= 1.0f) ||
      !(normal_scale >= 0.0f && normal_scale <= kMostScale) || !(power >= 1.0f && power < 1000.0f)) {
    out.why = "a value out of its range";
    return out;
  }
  out.ok = true;
  return out;
}
// [rexglue-native] 2026-11-10 rn40: glass with an environment cube (The Mace's goggle lenses, "c_papajack_lens_c" with
// Env_Map_Test_Texture "canyon2", a cube). The game draws it with the per-pixel lit models' pixel shader
// FC767C09DBD34C2A (acct2-bro's NOTES, 2026-11-05; its microcode in the work folder's acct2-bro-shaders), blended by
// alpha (rn40: blend 07060706; the opaque draws log 00010001). Its pass binds (rn40's rn25 lines) c16 to the value
// block's +1D0 (x: the cube's bias, 0), c17-c19 the light block's point light, c20-c28 its irradiance rows, c29 +660
// the sun's place (about 2000 units from the object toward the sun), c30 +720 its color (0.905 0.868 0.749 and 0.773
// 0.742 0.640 for two soldiers), c31 +C90 (x: the reflection's scale, 0.6); the highlights' power is its literal c255.w.
// rn40e: the Polish infantry's eyes (material: Reflection_Cube_Map_Texture "canyon2", Normal_Map_Texture
// "c_polinf_head1_n", the head's color map "c_polinf_head1_c"; rn40e-rn40m took them for the helmets, rn40n-rn40p
// found them a thin piece at the front of the face) are the game's pixel shader 5931940F174EA055 (rn40d: host draws
// reading c_polinf_head1_n; microcode in acct2-bro-shaders). It is opaque (blend 00010001), its normal map
// fetched but only its z used (gone in the normalize), so the vertex normal; its pass binds (rn40d's rn25 line) c16
// the eye, c17 +1C0 (x: the cube's bias, 0), c18-c20 the point light, c21-c29 the rows, c30-c33 the sun shadow's
// matrix, c34 +5F0 the sun's place, c35 +6B0 its color, c36 +A60 COD_SHADOW_FARZ (0.001), c37 +B20
// COD_SHADOW_GREYLEVEL (0.4); its literals: highlights of power 500 (c255.y), the cube x 0.072696 (c255.x), the
// highlights and the cube x (0.882, 1, 0.990363) (c254.y, 1, c254.x). The sun is the treads' (B2E0DA00):
// max(mix(its color x the 3 x 3 shadow, 1, saturate(FARZ x the view depth)), GREYLEVEL).
struct LensRegisters {
  const char* shader;
  bool opaque;                  // false: FC767C09's glass, blended by the color map's alpha
  uint32_t rows;                // the rows' first register
  uint32_t bias, place, color;  // registers
  uint32_t reflection;          // its register, 0: kReflection
  uint32_t fade, floor;         // registers, 0: no shadow (FC767C09 reads none)
  float power, reflection_literal, tint[3];
};
constexpr LensRegisters kLensGlass = {"FC767C09DBD34C2A", false, 20, 16, 29, 30, 31, 0, 0, 300.0f, 0.0f, {1, 1, 1}};
constexpr LensRegisters kLensEyes = {"5931940F174EA055", true, 21, 17, 34, 35, 0, 36, 37, 500.0f, 0.072696f,
                                     {0.882f, 1.0f, 0.990363f}};
// (why is set whenever ok is false, for the debug log)
struct LensPass {
  bool ok = false;
  const char* why = "";
  const LensRegisters* registers = nullptr;
  float bias = 0.0f, reflection = 0.0f, fade = 0.0f, floor = 0.0f;
  float sun_place[3] = {}, sun_color[3] = {};
};
LensPass ReadLensPass(uint8_t* base, uint32_t node, const LensRegisters& registers) {
  LensPass out;
  out.registers = &registers;
  const PassBinding binding = ReadPassBinding(base, node);
  if (!binding.ok || !binding.values) {
    out.why = "the pass's binding or values can't be read";
    return out;
  }
  const uint32_t last = registers.floor ? registers.floor : registers.reflection;  // the last from the value block
  if (binding.rows_at != registers.rows || last + 1 >= PassBinding::kMaxRegisters || binding.slot[last + 1] == 1) {
    out.why = "not bound as that shader (its rows' register, or more from the value block after its last)";
    return out;
  }
  out.reflection = registers.reflection_literal;
  if (!ReadBound(base, binding, registers.bias, &out.bias, 1) ||
      !ReadBound(base, binding, registers.place, out.sun_place, 3) ||
      !ReadBound(base, binding, registers.color, out.sun_color, 3) ||
      (registers.reflection && !ReadBound(base, binding, registers.reflection, &out.reflection, 1)) ||
      (registers.fade && (!ReadBound(base, binding, registers.fade, &out.fade, 1) ||
                          !ReadBound(base, binding, registers.floor, &out.floor, 1)))) {
    out.why = "a register isn't bound to the value block, or isn't a number";
    return out;
  }
  constexpr float kMostColor = 64.0f, kMostBias = 4.0f, kMostReflection = 16.0f;
  for (int i = 0; i < 3; ++i) {
    if (FillValue(out.sun_place[i]) || !(out.sun_color[i] >= 0.0f && out.sun_color[i] <= kMostColor)) {
      out.why = "the sun's place is the heap's fill, or its color is out of range";
      return out;
    }
  }
  if (FillValue(out.bias) || !(std::fabs(out.bias) <= kMostBias) ||
      !(out.reflection >= 0.0f && out.reflection <= kMostReflection) || !(out.fade >= 0.0f && out.fade < 1.0f) ||
      !(out.floor >= 0.0f && out.floor <= 1.0f)) {
    out.why = "the cube's bias, the reflection's scale, the fade or the floor is out of its range";
    return out;
  }
  out.ok = true;
  return out;
}
// [rexglue-native] 2026-11-10 rn40t: the Polish infantry's helmets (the color map "c_polinf_head1_c" with its normal
// map and a specular map, drawn as their own object at the soldier's place; rn40r's flat-white check) are the game's
// pixel shader 1D4725CEF0551EBB (vertex shader 85DA0D7EC65AC77C; rn40d's host draws). rn40n/rn40p: 5931940F174EA055
// (kLensEyes above) draws the eyes, not the helmets. Before rn40t they were drawn as other bumped models, with the
// light set's sun (0.373 0.358 0.309: the scene's / 2.4 x the light grid's share) where the game's is the pass's
// PROJCOLOR (0.905 0.868 0.749), and no shadow floor or fade: darker than the game's.
// Its pass binds (rn40s's rn25 line) c16 the eye's place (slot 2 +270), c17 / c18 the nearest point light's place and
// color (the light block's +40 / +30), c19-c27 the rows (its +F0), then from the value block c28 position_Light0
// (+5D0), c29 PROJCOLOR (+690), c30 COD_SHADOW_FARZ (+A40), c31 COD_SHADOW_GREYLEVEL (+B00): the soldiers' uniforms'
// registers (21168F53, kSunRegisters) at the skin's offsets. From its microcode (work folder acct2-bro-shaders) and its
// literals at its draws (rn40d: c253 (13.662 2.38486 3 0), c254 (-1 1 0.5 2), c255 (13.662 1.737 0 1)):
//   - the normal map's (2A - 1, 2G - 1, sqrt(|1 - x^2 - y^2|)), unscaled, on the frame from the screen slopes
//     (instructions 10-22, 31-71);
//   - the base x mix(1.737, 1, s), s = t^2 (3 - 2t), t = saturate(N.V) (76, 78, 81-82, 85, 88-89): the skin's form
//     (3949D29E's 1.215);
//   - the sun = max(mix(PROJCOLOR x the 3 x 3 shadow, 1, saturate(FARZ x the view depth)), GREYLEVEL), its way from
//     position_Light0 (62-64, 73, 83, 92, 103-108);
//   - the light = the nine rows + saturate(N.L) x the sun + saturate(N.L') x the point light's color x its falloff
//     (72, 75-98, 110-111);
//   - the highlights = (the sun x saturate(N.H)^13.662 + the point light's x saturate(N.H')^13.662) x 2.38486 x the
//     specular map, no N.L (105, 108-109, 112-115);
//   - out = the light x the base + the highlights (116).
// That is the skin's (ngl_vk.h noise_swatch) with no noise swatch, so the helmets are drawn as the skin with these
// numbers and the swatch's share 0. Told apart by the pass's binding (HelmetLayout): rows from c19, c16 the eye, c17 and
// c18 the point light, c28-c31 from the value block and nothing from it at c32 (the gun metal, CFC45995D1294CCE, also
// has its rows from c19 but binds c32). ok when all four are read and in their ranges.
constexpr uint32_t kRowsHelmet = 19;
constexpr uint32_t kHelmetEye = 16, kHelmetPointPlace = 17, kHelmetPointColor = 18;
constexpr uint32_t kHelmetPlace = 28, kHelmetColor = 29, kHelmetFade = 30, kHelmetFloor = 31;
constexpr float kHelmetEdgeLift = 1.737f;           // c255.y
constexpr float kHelmetSpecularPower = 13.662f;     // c253.x (and c255.x)
constexpr float kHelmetSpecularScale = 2.38486f;    // c253.y
// [rexglue-native] 2026-11-10 rn40x: ADC02DCE4FDC9253 binds the same registers (rn40v took papajack's and bangboom's
// bodies, c_*_m_body_c, as helmets), and the near Polish infantry soldier's helmet (material ADEA4440) is drawn by it
// too, not by 1D4725CE: rn40w listed the materials' samplers, which are the shaders' tf2 on in order. 1D4725CE's hold
// Normal_Map, Diffuse_Map, Specular_Map (its tf2 normal, tf3 color, tf4 specular); ADC02DCE's hold Normal_Map,
// Normal_Swatch, Diffuse_Map, Noise_Swatch, Specular_Map (its tf2-tf6). From ADC02DCE's microcode (acct2-bro-shaders)
// and literals (rn40d: c252 (1.737 28.717 3.641 0), c253 (23.783 13.662 2 3), c254 (-1 1 0.5 -0.5), c255 (0.655
// 13.662 0 1)), it is 1D4725CE's with:
//   - the Normal_Swatch at (uv - 0.5) x 28.717 + 0.5 folded into the normal map whole (instructions 11-29: the
//     uniforms' fold with the mask at 1, both z's as sqrt(|1 - x^2 - y^2|));
//   - the highlights' mask mix(3.641 x the specular map, the Noise_Swatch at (uv - 0.5) x 23.783 + 0.5, 1 - 0.655)
//     (60-62, 75, 77, 122, 125) in place of 2.38486 x the specular map. The base x mix(1.737, 1, s), the sun, the
//     light and the highlights' power 13.662 are the same.
constexpr float kWovenSwatchRepeat = 28.717f;       // c252.y
constexpr float kWovenSpecularScale = 3.641f;       // c252.z
constexpr float kWovenNoiseRepeat = 23.783f;        // c253.x
constexpr float kWovenNoiseMix = 1.0f - 0.655f;     // 1 - c255.x
bool HelmetLayout(const PassBinding& binding) {
  if (binding.rows_at != kRowsHelmet || binding.slot[kHelmetEye] != 2 || binding.slot[kHelmetPointPlace] != 4 ||
      binding.offset[kHelmetPointPlace] != 0x40 || binding.slot[kHelmetPointColor] != 4 ||
      binding.offset[kHelmetPointColor] != 0x30 || binding.slot[kHelmetFloor + 1] == 1) {
    return false;
  }
  for (uint32_t c = kHelmetPlace; c <= kHelmetFloor; ++c) {
    if (binding.slot[c] != 1) return false;
  }
  return true;
}
// (why is set whenever ok is false, for the debug log)
struct HelmetPass {
  bool ok = false;
  const char* why = "";
  float sun_place[3] = {}, sun_color[3] = {};
  float shadow_fade = 0.0f, shadow_floor = 0.0f;
};
HelmetPass ReadHelmetPass(uint8_t* base, uint32_t node) {
  HelmetPass out;
  const PassBinding binding = ReadPassBinding(base, node);
  if (!binding.ok || !binding.values) {
    out.why = "the pass's binding or values can't be read";
    return out;
  }
  if (!HelmetLayout(binding)) {
    out.why = "not bound as 1D4725CEF0551EBB";
    return out;
  }
  if (!ReadBound(base, binding, kHelmetPlace, out.sun_place, 3) ||
      !ReadBound(base, binding, kHelmetColor, out.sun_color, 3) ||
      !ReadBound(base, binding, kHelmetFade, &out.shadow_fade, 1) ||
      !ReadBound(base, binding, kHelmetFloor, &out.shadow_floor, 1)) {
    out.why = "a register isn't bound to the value block, or isn't a number";
    return out;
  }
  constexpr float kMostColor = 64.0f;
  for (int i = 0; i < 3; ++i) {
    if (FillValue(out.sun_place[i]) || !(out.sun_color[i] >= 0.0f && out.sun_color[i] <= kMostColor)) {
      out.why = "the sun's place is the heap's fill, or its color is out of range";
      return out;
    }
  }
  if (!(out.shadow_fade >= 0.0f && out.shadow_fade < 1.0f) ||
      !(out.shadow_floor >= 0.0f && out.shadow_floor <= 1.0f)) {
    out.why = "the fade or the floor is out of its range";
    return out;
  }
  out.ok = true;
  return out;
}
// [rexglue-native] 2026-11-10 rn40y: the Polish infantry's heads drawn without a specular map (materials ADEA4F20 and
// ADEA12C0, on the soldiers away from the camera; rn40s-rn40x's watched generic bumped draws) are the game's pixel
// shader 2340DF59702A9AD4 (vertex shader FE02FFA1, skinned). Its pass binds (rn40s's rn25 line) c16 the nearest point
// light's color (the light block's +30), c17-c25 the rows (+F0), then from the value block c26 PROJCOLOR (+6B0), c27
// COD_SHADOW_FARZ (+A60), c28 COD_SHADOW_GREYLEVEL (+B20), and c29-c31 from slot 6 (the shadow map's); the sun's way
// is its vertex shader's (o5: c168 - the position), not bound here, so the native draw keeps the light's own way. From
// its microcode (work folder acct2-bro-shaders) and literals (rn40d: c254 (2 0.2 3 5), c255 (0.5 -1 0 1)):
//   - the normal map's G and A, unscaled, z = sqrt(|1 - x^2 - y^2|), on the frame from the screen slopes (instructions
//     9-16, 21-24, 39-78);
//   - the sun = max(mix(PROJCOLOR x the 3 x 3 shadow, 1, saturate(FARZ x the view depth)), GREYLEVEL) (89-90, 93-94,
//     97), times saturate(N.L) and a soft terminator s = t^2 (3 - 2t), t = saturate(5 x (the vertex normal . L + 0.2))
//     (45-60, 98, 100); the point light the same, with its own way and falloff (76-83, 101);
//   - out = (the nine rows + those) x the color map (102): no highlights, no edge lift.
// Drawn as the skin with every extra off (edge lift 1, no highlights, no noise) and ngl_vk.h's skin_terminator.
constexpr uint32_t kRowsPlainHead = 17;
constexpr uint32_t kPlainHeadPointColor = 16, kPlainHeadColor = 26, kPlainHeadFade = 27, kPlainHeadFloor = 28;
constexpr float kPlainHeadTerminatorBias = 0.2f;    // c254.y
constexpr float kPlainHeadTerminatorScale = 5.0f;   // c254.w
struct PlainHeadPass {
  bool ok = false;
  const char* why = "";
  float sun_color[3] = {};
  float shadow_fade = 0.0f, shadow_floor = 0.0f;
};
PlainHeadPass ReadPlainHeadPass(uint8_t* base, uint32_t node) {
  PlainHeadPass out;
  const PassBinding binding = ReadPassBinding(base, node);
  if (!binding.ok || !binding.values) {
    out.why = "the pass's binding or values can't be read";
    return out;
  }
  if (binding.rows_at != kRowsPlainHead || binding.slot[kPlainHeadPointColor] != 4 ||
      binding.offset[kPlainHeadPointColor] != 0x30 || binding.slot[kPlainHeadColor] != 1 ||
      binding.slot[kPlainHeadFade] != 1 || binding.slot[kPlainHeadFloor] != 1 ||
      binding.slot[kPlainHeadFloor + 1] == 1) {
    out.why = "not bound as 2340DF59702A9AD4";
    return out;
  }
  if (!ReadBound(base, binding, kPlainHeadColor, out.sun_color, 3) ||
      !ReadBound(base, binding, kPlainHeadFade, &out.shadow_fade, 1) ||
      !ReadBound(base, binding, kPlainHeadFloor, &out.shadow_floor, 1)) {
    out.why = "a register isn't bound to the value block, or isn't a number";
    return out;
  }
  constexpr float kMostColor = 64.0f;
  for (int i = 0; i < 3; ++i) {
    if (!(out.sun_color[i] >= 0.0f && out.sun_color[i] <= kMostColor)) {
      out.why = "the sun's color is out of range";
      return out;
    }
  }
  if (!(out.shadow_fade >= 0.0f && out.shadow_fade < 1.0f) ||
      !(out.shadow_floor >= 0.0f && out.shadow_floor <= 1.0f)) {
    out.why = "the fade or the floor is out of its range";
    return out;
  }
  out.ok = true;
  return out;
}
constexpr uint32_t kParameterValue = 0x14;

// ParameterName, or the name one pointer deep (side29: the parameter entries' own words hold no string; their first
// two words point into the effect).
// The printable text right at `at` (a descriptor is {hash, name in place}), "" when it isn't 3 or more characters.
std::string InlineName(uint8_t* base, uint32_t at) {
  if (!Readable(at)) return "";
  std::string name;
  const uint8_t* text = GpuPointer(base, at);  // side164: as GpuU32 (an unmapped 0xA0000000-view page)
  for (uint32_t k = 0; k < 40; ++k) {
    const uint8_t c = text[k];
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) return "";
    name += static_cast<char>(c);
  }
  return name.size() >= 3 ? name : "";
}

std::string DeepName(uint8_t* base, uint32_t at) {
  std::string name = ParameterName(base, at);
  if (name != "-") return name;
  if (!(name = InlineName(base, at + 4)).empty()) return "\"" + name + "\" in place";
  for (uint32_t k = 0; k < 0x10; k += 4) {
    const uint32_t inner = g::ReadU32(base, at + k);
    if (!Pointer(inner) || !Readable(inner)) continue;
    name = ParameterName(base, inner);
    if (name != "-") return "+" + std::to_string(k) + " -> " + name;
    if (!(name = InlineName(base, inner + 4)).empty())
      return "+" + std::to_string(k) + " -> \"" + name + "\" in place";
  }
  return "-";
}

void ShowShaderState(uint8_t* base, uint32_t state) {
  static std::unordered_set<uint32_t> shown;
  if (!Pointer(state) || !Readable(state) || shown.count(state) || shown.size() >= 12) return;
  shown.insert(state);
  REXLOG_INFO("cod3: ngl side: state {:08X}: {} / {}", state, Words(base, state, 8), Words(base, state + 0x20, 8));
  for (uint32_t at : {0x20u, 0x24u}) {  // the effect's objects
    const uint32_t effect = g::ReadU32(base, state + at);
    if (Pointer(effect) && Readable(effect))
      REXLOG_INFO("cod3: ngl side: state {:08X} +{:02X} effect {:08X}: {} / {}", state, at, effect,
                  Words(base, effect, 8), Words(base, effect + 0x20, 8));
  }
  uint32_t parameter = g::ReadU32(base, state + kStateParameters);
  for (int k = 0; k < 24 && Pointer(parameter) && Readable(parameter); ++k) {
    const uint32_t code = g::ReadU32(base, parameter + kParameterCode);
    const uint32_t value = g::ReadU32(base, parameter + kParameterValue);
    std::string floats = "-";
    if (Pointer(value) && Readable(value) && Readable(value + 12)) {
      char text[128];
      std::snprintf(text, sizeof text, "(%g %g %g %g)", g::ReadF32(base, value), g::ReadF32(base, value + 4),
                    g::ReadF32(base, value + 8), g::ReadF32(base, value + 12));
      floats = text;
    }
    const uint32_t described = g::ReadU32(base, parameter);
    REXLOG_INFO("cod3: ngl side: state {:08X} parameter {}: code {:02X}, value at {:08X} {}, name {}, words {}; "
                "described at {:08X}: {}",
                state, k, code, value, floats, DeepName(base, parameter), Words(base, parameter, 8), described,
                Pointer(described) && Readable(described) ? Words(base, described, 8) : std::string("-"));
    parameter = g::ReadU32(base, parameter + kParameterNext);
  }
}

// Any shader state's parameters named like fog (FogColor is on some effects, side29), each name once, to find the
// haze's color and distances: "fog parameter N on state S: code C, value (a b c d)".
void HuntFog(uint8_t* base, uint32_t state) {
  static std::unordered_set<std::string> shown;
  static std::unordered_set<uint32_t> states;
  if (shown.size() >= 12 || states.size() >= 4096 || !Pointer(state) || !Readable(state) ||
      !states.insert(state).second)
    return;
  uint32_t parameter = g::ReadU32(base, state + kStateParameters);
  for (int k = 0; k < 64 && Pointer(parameter) && Readable(parameter); ++k) {
    const std::string name = DeepName(base, parameter);
    std::string lower;
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("fog") != std::string::npos && shown.insert(name).second) {
      const uint32_t value = g::ReadU32(base, parameter + kParameterValue);
      std::string floats = "-";
      if (Pointer(value) && Readable(value) && Readable(value + 12)) {
        char text[128];
        std::snprintf(text, sizeof text, "(%g %g %g %g)", g::ReadF32(base, value), g::ReadF32(base, value + 4),
                      g::ReadF32(base, value + 8), g::ReadF32(base, value + 12));
        floats = text;
      }
      REXLOG_INFO("cod3: ngl side: fog parameter {} on state {:08X}: code {:02X}, value at {:08X} {}", name, state,
                  g::ReadU32(base, parameter + kParameterCode), value, floats);
    }
    parameter = g::ReadU32(base, parameter + kParameterNext);
  }
}

// side181: the world's two nearby lights. The lightmapped world's pixel shaders take LIGHT_POSITION1/2, LIGHT_COLOR1/2
// and the vertex shaders LIGHT_INVFAR1/2 (side165's names); a shader state's parameters named like a light, each name
// with its first 3 values, to find where the game keeps them: "light parameter N on state S: code C, value (a b c d)".
void HuntLights(uint8_t* base, uint32_t state) {
  static std::unordered_map<std::string, int> shown;
  static std::unordered_set<uint32_t> states;
  if (shown.size() >= 24 || states.size() >= 4096 || !Pointer(state) || !Readable(state) ||
      !states.insert(state).second)
    return;
  uint32_t parameter = g::ReadU32(base, state + kStateParameters);
  for (int k = 0; k < 64 && Pointer(parameter) && Readable(parameter); ++k) {
    const std::string name = DeepName(base, parameter);
    std::string lower;
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("light") != std::string::npos && shown[name]++ < 3) {
      const uint32_t value = g::ReadU32(base, parameter + kParameterValue);
      std::string floats = "-";
      if (Pointer(value) && Readable(value) && Readable(value + 12)) {
        char text[128];
        std::snprintf(text, sizeof text, "(%g %g %g %g)", g::ReadF32(base, value), g::ReadF32(base, value + 4),
                      g::ReadF32(base, value + 8), g::ReadF32(base, value + 12));
        floats = text;
      }
      REXLOG_INFO("cod3: ngl side: light parameter {} on state {:08X}: code {:02X}, value at {:08X} {}", name, state,
                  g::ReadU32(base, parameter + kParameterCode), value, floats);
    }
    parameter = g::ReadU32(base, parameter + kParameterNext);
  }
}

// The sun as the game's shaders get it, from a shader state's parameter list (side29-30, COD3-NGL-MAP.md):
// position_light0 / cod_shadow_position (code 0x35: the sun's position, its direction times 10^6; a point light's is
// a plain position, left out here), cod_shadow_projcolor (0x37: its color) and __40_temp1_cmn (0x2A: 0.39 gray in the
// forest), taken as the ambient when the list has it.
constexpr uint32_t kCodeLightPosition = 0x35;
constexpr uint32_t kCodeLightColor = 0x37;
constexpr uint32_t kCodeAmbient = 0x2A;
constexpr float kSunFarAway = 1e5f;      // longer than this: a direction, not a place
constexpr float kDefaultAmbient = 0.35f;
// [rexglue-native] 2026-11-09 rn18: a parameter value still holding the fill 0xCDCDCDCD was never written. The Mace's
// states (a storm, no sun) have their sun position and color there: read as floats, -4.3e8 each, so the sun "traveled"
// 0.577 0.577 0.577 with a color of -4.3e8, and with cod3_debug_ngl_side_model_light 2 every soldier's and vehicle's
// first light became that (rn16d, rn17b: the soldiers far darker than the game's). Such a value, or a sun color outside
// 0..kSunColorMost, is no sun.
constexpr uint32_t kUnwrittenFill = 0xCDCDCDCD;
constexpr float kSunColorMost = 64.0f;

struct Sun {
  bool ok = false;
  float direction[3] = {0, 0, -1};  // the way the light travels
  float color[3] = {1, 1, 1};
  float ambient[3] = {kDefaultAmbient, kDefaultAmbient, kDefaultAmbient};
  // side99: up to three more directional lights (a light set's next strongest), as the game's vertex shaders light by
  // four (shader_C4130C72D4721102: c4-c7 the directions, c8-c10 their colors, c11 the ambient, saturated)
  uint32_t more = 0;
  float more_direction[3][3] = {};
  float more_color[3][3] = {};
};
Sun g_sun;  // the last one a state had, for draws whose own state has none
// side108: the way the sun travels, from the last light set with a directional light (a soldier's: the sun the light
// grid weights), for the native sun's shadow
float g_sun_way[3] = {0, 0, -1};
bool g_sun_way_known = false;

bool StateSun(uint8_t* base, uint32_t state, Sun& sun) {
  if (!Pointer(state) || !Readable(state)) return false;
  bool placed = false, colored = false, refused = false;
  uint32_t parameter = g::ReadU32(base, state + kStateParameters);
  for (int k = 0; k < 24 && Pointer(parameter) && Readable(parameter); ++k) {
    const uint32_t code = g::ReadU32(base, parameter + kParameterCode);
    const uint32_t value = g::ReadU32(base, parameter + kParameterValue);
    refused = refused || ((code == kCodeLightPosition || code == kCodeLightColor) && Pointer(value) &&
                          Readable(value) && g::ReadU32(base, value) == kUnwrittenFill);
    const bool unwritten = Pointer(value) && Readable(value) && Readable(value + 12) &&  // rn18
                           (g::ReadU32(base, value) == kUnwrittenFill || g::ReadU32(base, value + 4) == kUnwrittenFill ||
                            g::ReadU32(base, value + 8) == kUnwrittenFill);
    if (Pointer(value) && Readable(value) && Readable(value + 12) && !unwritten) {
      const float v[3] = {g::ReadF32(base, value), g::ReadF32(base, value + 4), g::ReadF32(base, value + 8)};
      const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      const bool color_ok = v[0] >= 0.0f && v[1] >= 0.0f && v[2] >= 0.0f && v[0] <= kSunColorMost &&
                            v[1] <= kSunColorMost && v[2] <= kSunColorMost;  // rn18 (false for a NaN too)
      if (code == kCodeLightPosition && !placed && std::isfinite(length) && length > kSunFarAway) {
        for (int i = 0; i < 3; ++i) sun.direction[i] = -v[i] / length;
        placed = true;
      } else if (code == kCodeLightColor && !colored && std::isfinite(length) && color_ok) {
        for (int i = 0; i < 3; ++i) sun.color[i] = v[i];
        colored = true;
      } else if (code == kCodeAmbient && std::isfinite(length)) {
        for (int i = 0; i < 3; ++i) sun.ambient[i] = v[i];
      }
    }
    parameter = g::ReadU32(base, parameter + kParameterNext);
  }
  sun.ok = placed && colored;
  static std::atomic<int> refused_shown{0};  // rn18
  if (refused && refused_shown.fetch_add(1, std::memory_order_relaxed) < 2) {
    REXLOG_INFO("cod3: ngl side: state {:08X}'s sun was never written (0xCDCDCDCD): no scene sun from it", state);
  }
  return sun.ok;
}

// side89: a surface's own lights. Like Big Red One's nglListAddMesh, a surface's render parameters (object +0x84) can
// hold a light set (the light-set parameter, its number at 0x8290F2B0; cod3mp 0x82A8C1C4): the soldiers' (0x82427D18
// passes theirs, made from the lighting where they stand) and the instanced props' (0x820D27E0, Big Red One's
// cdSimpleInstance_AddListToScene: nglCreateLightSet 0x82150A20, one directional light 0x82150A88, the ambient
// 0x8214FEC0). The game's lighting (0x82142360) walks that set, so a soldier in the trees' shade is dark, where the
// one sun lit every soldier alike. A set is 0x70 bytes: +0x00 eight light lists (one per light group, each ending at
// the set), +0x60 the ambient (red, green, blue, clamped to 0..1). A light: +4 x group the next in that group, +0x28
// its kind (1 directional), +0x2C its data: +0x00 the way it travels, +0x10 its color. Here the strongest directional
// light of any group is the draw's light and the others are added to the ambient at half, as for Big Red One
// (ngl_mesh.c read_lights). side99: the four strongest are the draw's lights, as the game's vertex shaders take four
// (only the ones after those go to the ambient at half).
#if defined(COD3_MP)
constexpr uint32_t kLightSetParameter = 0x82A8C1C4;  // mp/globals_sp_to_mp.tsv: 6 of 6 uses
#else
constexpr uint32_t kLightSetParameter = 0x8290F2B0;
#endif
constexpr uint32_t kLightSetAmbient = 0x60;
constexpr uint32_t kLightGroups = 8;
constexpr uint32_t kLightKind = 0x28;
constexpr uint32_t kLightData = 0x2C;
constexpr uint32_t kLightDirectional = 1;
constexpr uint32_t kMaxSetLights = 16;

// [rexglue-native] 2026-11-10 rn40j: an object's light set (its render parameters' light-set parameter), 0 for none
uint32_t ObjectLightSet(uint8_t* base, uint32_t object) {
  if (!Pointer(object)) return 0;
  const uint32_t bit = g::ReadU32(base, kLightSetParameter);  // a global of the image
  const uint32_t parameters =
      Readable(object + kObjectRenderParameters) ? g::ReadU32(base, object + kObjectRenderParameters) : 0;
  if (bit >= 64 || !Readable(parameters) || !Readable(parameters + 4 * (bit + 2))) return 0;
  const uint64_t mask = uint64_t{g::ReadU32(base, parameters)} << 32 | g::ReadU32(base, parameters + 4);
  if (!((mask >> bit) & 1)) return 0;
  const uint32_t set = g::ReadU32(base, parameters + 4 * (bit + 2));
  return Pointer(set) ? set : 0;
}

// rn40j: a model's place (its world matrix's translation, object +0x30); false when it can't be read or isn't sane
bool ObjectPlace(uint8_t* base, uint32_t object, float at[3]) {
  if (!Pointer(object) || !Readable(object + 0x3C)) return false;
  for (int i = 0; i < 3; ++i) {
    at[i] = g::ReadF32(base, object + 0x30 + 4 * i);
    if (!std::isfinite(at[i]) || std::fabs(at[i]) > 1.0e6f) return false;
  }
  return true;
}

bool ObjectSun(uint8_t* base, uint32_t object, Sun& sun) {
  const uint32_t bit = g::ReadU32(base, kLightSetParameter);  // a global of the image
  const uint32_t parameters =
      Readable(object + kObjectRenderParameters) ? g::ReadU32(base, object + kObjectRenderParameters) : 0;
  if (bit >= 64 || !Readable(parameters) || !Readable(parameters + 4 * (bit + 2))) return false;
  const uint64_t mask = uint64_t{g::ReadU32(base, parameters)} << 32 | g::ReadU32(base, parameters + 4);
  if (!((mask >> bit) & 1)) return false;
  const uint32_t set = g::ReadU32(base, parameters + 4 * (bit + 2));
  if (!Pointer(set) || !Readable(set + kLightSetAmbient + 12)) return false;
  Sun own;
  for (uint32_t i = 0; i < 3; ++i) own.ambient[i] = g::ReadF32(base, set + kLightSetAmbient + 4 * i);
  uint32_t seen[kMaxSetLights];
  uint32_t seen_count = 0;
  struct Light {
    float direction[3], color[3], luminance;
  } lights[kMaxSetLights];
  uint32_t light_count = 0;
  for (uint32_t group = 0; group < kLightGroups; ++group) {
    uint32_t light = g::ReadU32(base, set + 4 * group);
    for (uint32_t n = 0; light != set && Pointer(light) && Readable(light + kLightData) && n < kMaxSetLights;
         light = g::ReadU32(base, light + 4 * group), ++n) {
      bool again = false;
      for (uint32_t k = 0; k < seen_count; ++k) again = again || seen[k] == light;
      if (again || seen_count == kMaxSetLights) continue;
      seen[seen_count++] = light;
      const uint32_t data = g::ReadU32(base, light + kLightData);
      const uint32_t kind = g::ReadU32(base, light + kLightKind);
      static int others_shown = 0;
      if (others_shown < 8 && Pointer(data) && Readable(data + 0x8C)) {
        // side100-101: what each light of a set holds (the game's lighting, 0x82142360, takes kind 3 as the sun,
        // kind 8 as nine four-float rows it adds the ambient and the other directional lights into, kinds 0 and 5 as
        // the two nearest point lights)
        ++others_shown;
        std::string rows;
        for (uint32_t r = 0; r < 9; ++r) {
          char row[96];
          std::snprintf(row, sizeof row, " (%.4g %.4g %.4g %.4g)", g::ReadF32(base, data + 16 * r),
                        g::ReadF32(base, data + 16 * r + 4), g::ReadF32(base, data + 16 * r + 8),
                        g::ReadF32(base, data + 16 * r + 12));
          rows += row;
        }
        REXLOG_INFO("cod3: ngl side: light set {:08X}: light {:08X} of kind {}, its data {:08X}:{}", set, light, kind,
                    data, rows);
      } else if (others_shown < 8) {
        ++others_shown;
        REXLOG_INFO("cod3: ngl side: light set {:08X}: light {:08X} of kind {}, its data {:08X} (not readable)", set,
                    light, kind, data);
      }
      if (kind != kLightDirectional || !Pointer(data) || !Readable(data + 0x1C)) continue;
      Light& l = lights[light_count];
      for (uint32_t i = 0; i < 3; ++i) {
        l.direction[i] = g::ReadF32(base, data + 4 * i);
        l.color[i] = g::ReadF32(base, data + 0x10 + 4 * i);
      }
      l.luminance = 0.3f * l.color[0] + 0.59f * l.color[1] + 0.11f * l.color[2];
      if (std::isfinite(l.luminance) && l.luminance > 0.0f) ++light_count;
    }
  }
  std::stable_sort(lights, lights + light_count,
                   [](const Light& a, const Light& b) { return a.luminance > b.luminance; });
  const bool found = light_count > 0;
  if (found) {
    std::memcpy(own.direction, lights[0].direction, sizeof own.direction);
    std::memcpy(own.color, lights[0].color, sizeof own.color);
    std::memcpy(g_sun_way, own.direction, sizeof g_sun_way);  // side108
    g_sun_way_known = true;
  }
  for (uint32_t k = 1; k < light_count; ++k) {
    if (k <= 3) {
      std::memcpy(own.more_direction[k - 1], lights[k].direction, sizeof own.more_direction[k - 1]);
      std::memcpy(own.more_color[k - 1], lights[k].color, sizeof own.more_color[k - 1]);
      own.more = k;
    } else {
      for (int i = 0; i < 3; ++i) own.ambient[i] += 0.5f * lights[k].color[i];
    }
  }
  if (!found) {
    for (float& c : own.color) c = 0.0f;
  }
  own.ok = true;
  static int shown = 0;
  if (shown < 8) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: object {:08X}'s own light set {:08X}: ambient {:.3f} {:.3f} {:.3f}, {} light(s), the "
                "strongest travels {:.3f} {:.3f} {:.3f}, color {:.3f} {:.3f} {:.3f}; {} more drawn",
                object, set, own.ambient[0], own.ambient[1], own.ambient[2], seen_count, own.direction[0],
                own.direction[1], own.direction[2], own.color[0], own.color[1], own.color[2], own.more);
    for (uint32_t k = 0; k < own.more; ++k)
      REXLOG_INFO("cod3: ngl side:   light {}: travels {:.3f} {:.3f} {:.3f}, color {:.3f} {:.3f} {:.3f}", k + 2,
                  own.more_direction[k][0], own.more_direction[k][1], own.more_direction[k][2],
                  own.more_color[k][0], own.more_color[k][1], own.more_color[k][2]);
  }
  sun = own;
  return true;
}

// side182: the world's nearby lights aren't shader-state parameters (side181: only the sun's are), so the game's
// lighting (0x82142360) must hand them from the object's light set, as a model's (kinds 0 and 5 the two nearest point
// lights, side100-101). For the lightmapped world: how many objects have a set, how many of those hold lights other
// than directional ones, and the first such lights' data (nine four-float rows).
std::atomic<uint32_t> g_world_sets{0}, g_world_point_sets{0};

void SurveyWorldLights(uint8_t* base, uint32_t object) {
  const uint32_t bit = g::ReadU32(base, kLightSetParameter);
  const uint32_t parameters =
      Readable(object + kObjectRenderParameters) ? g::ReadU32(base, object + kObjectRenderParameters) : 0;
  if (bit >= 64 || !Readable(parameters) || !Readable(parameters + 4 * (bit + 2))) return;
  const uint64_t mask = uint64_t{g::ReadU32(base, parameters)} << 32 | g::ReadU32(base, parameters + 4);
  if (!((mask >> bit) & 1)) return;
  const uint32_t set = g::ReadU32(base, parameters + 4 * (bit + 2));
  if (!Pointer(set) || !Readable(set + kLightSetAmbient + 12)) return;
  g_world_sets.fetch_add(1, std::memory_order_relaxed);
  bool point = false;
  static std::atomic<int> shown{0};
  for (uint32_t group = 0; group < kLightGroups; ++group) {
    uint32_t light = g::ReadU32(base, set + 4 * group);
    for (uint32_t n = 0; light != set && Pointer(light) && Readable(light + kLightData) && n < kMaxSetLights;
         light = g::ReadU32(base, light + 4 * group), ++n) {
      const uint32_t kind = g::ReadU32(base, light + kLightKind);
      const uint32_t data = g::ReadU32(base, light + kLightData);
      if (kind == kLightDirectional || kind == 8) continue;
      point = true;
      if (!Pointer(data) || !Readable(data + 0x8C) || shown.fetch_add(1, std::memory_order_relaxed) >= 12) continue;
      std::string rows;
      for (uint32_t r = 0; r < 9; ++r) {
        char row[96];
        std::snprintf(row, sizeof row, " (%.4g %.4g %.4g %.4g)", g::ReadF32(base, data + 16 * r),
                      g::ReadF32(base, data + 16 * r + 4), g::ReadF32(base, data + 16 * r + 8),
                      g::ReadF32(base, data + 16 * r + 12));
        rows += row;
      }
      REXLOG_INFO("cod3: ngl side: world object {:08X}'s light set {:08X}: group {} light {:08X} of kind {}, data "
                  "{:08X}:{}", object, set, group, light, kind, data, rows);
    }
  }
  if (point) g_world_point_sets.fetch_add(1, std::memory_order_relaxed);
}

#if defined(COD3_NGL_VK)  // the native draws' light and fog
// side183: a lightmapped world draw's two nearby lights, from its object's light set: the first two lit point lights
// (kind 0; data row 0 the place in the world, row 1 the color, row 2 .y the range), as the game's lighting
// (0x82142360) hands them to the shaders' LIGHT_POSITION1/2, LIGHT_COLOR1/2 and LIGHT_INVFAR1/2 (1 / the range);
// the world vertex shader 5B41B3324503BB90 fades each as saturate(1 - distance x LIGHT_INVFAR).
constexpr uint32_t kLightPoint = 0;

void NearbyLights(uint8_t* base, uint32_t object, ngl_vk_mesh_draw& d) {
  d.point_light_count = 0;
  const uint32_t bit = g::ReadU32(base, kLightSetParameter);
  const uint32_t parameters =
      Readable(object + kObjectRenderParameters) ? g::ReadU32(base, object + kObjectRenderParameters) : 0;
  if (bit >= 64 || !Readable(parameters) || !Readable(parameters + 4 * (bit + 2))) return;
  const uint64_t mask = uint64_t{g::ReadU32(base, parameters)} << 32 | g::ReadU32(base, parameters + 4);
  if (!((mask >> bit) & 1)) return;
  const uint32_t set = g::ReadU32(base, parameters + 4 * (bit + 2));
  if (!Pointer(set) || !Readable(set + kLightSetAmbient + 12)) return;
  uint32_t seen[kMaxSetLights];
  uint32_t seen_count = 0;
  for (uint32_t group = 0; group < kLightGroups && d.point_light_count < 2; ++group) {
    uint32_t light = g::ReadU32(base, set + 4 * group);
    for (uint32_t n = 0; light != set && Pointer(light) && Readable(light + kLightData) && n < kMaxSetLights &&
                         d.point_light_count < 2;
         light = g::ReadU32(base, light + 4 * group), ++n) {
      bool again = false;
      for (uint32_t k = 0; k < seen_count; ++k) again = again || seen[k] == light;
      if (again || seen_count == kMaxSetLights) continue;
      seen[seen_count++] = light;
      const uint32_t data = g::ReadU32(base, light + kLightData);
      if (g::ReadU32(base, light + kLightKind) != kLightPoint || !Pointer(data) || !Readable(data + 0x2C)) continue;
      float place[3], color[3];
      for (uint32_t i = 0; i < 3; ++i) {
        place[i] = g::ReadF32(base, data + 4 * i);
        color[i] = g::ReadF32(base, data + 0x10 + 4 * i);
      }
      const float range = g::ReadF32(base, data + 0x24);
      const float brightness = color[0] + color[1] + color[2];
      if (!(range > 0.0f) || !std::isfinite(range) || !(brightness > 0.0f) || !std::isfinite(brightness) ||
          !std::isfinite(place[0] + place[1] + place[2]))
        continue;
      float* l = d.point_light[d.point_light_count++];
      for (int i = 0; i < 3; ++i) {
        l[i] = place[i];
        l[4 + i] = color[i];
      }
      l[3] = 1.0f / range;
    }
  }
}

// side184: a lit model's native draw takes the light the game handed its own draws lately
// (cod3_debug_ngl_side_model_irradiance): the irradiance rows turned into the mesh's own space, the nearest point
// light, and no extra lights (the rows hold them). The rows are a form in the world normal, n' M n + b . n (M's
// cross terms halved); with R the world's rotation (its first three rows made unit, each the mesh's axis in the
// world), the world normal is R' times the mesh's, so the mesh's form is R M R' and R b. False when there is none.
// rn40j: when its own object has none lately (an object made anew each frame), the light kept lately for a model drawn
// where it stands (cod3_debug_ngl_side_light_by_place): of the kept lights within kKeptLightNear of its place, one of
// its own light set first, else the nearest; *by_place says it was taken so.
// rn42t: *kept_frame, when given, gets the frame the light taken was kept on (debug only). rn42w: *taken, when given,
// the whole light taken (its main light for cod3_debug_ngl_side_model_sun).
// rn43a: the light kept for an object standing at `at` (its own, else by place as below; at null: its own only), as
// GameLightFor takes it; false when there is none. Takes g_model_light_mutex.
bool FindKeptLight(uint8_t* base, uint32_t object, uint64_t frame, const float* at, GameModelLight& light,
                   bool* by_place, bool* own_none) {
  if (by_place) *by_place = false;
  if (own_none) *own_none = false;
  std::lock_guard<std::mutex> lock(g_model_light_mutex);
  const auto it = g_model_lights.find(object);
  // rn43c: and kept where the model stands now. Objects are made anew each frame and their addresses come round again
  // every few frames, so the light kept at its address may be another model's: rn43a's The Mace, a soldier's own
  // light kept two frames before at its address read 0.905 where his was 0.832, for one frame.
  // Both places are the objects' own (+0x30, ObjectPlace), as KeepModelLight keeps it.
  bool same_place = true;
  float here[3] = {};
  if (it != g_model_lights.end() && it->second.placed && REXCVAR_GET(cod3_debug_ngl_side_own_light_place) &&
      ObjectPlace(base, object, here)) {
    float distance = 0.0f;
    for (int i = 0; i < 3; ++i) distance += (it->second.at[i] - here[i]) * (it->second.at[i] - here[i]);
    same_place = distance <= kKeptLightNear * kKeptLightNear;
  }
  if (it != g_model_lights.end() && frame <= it->second.frame + kModelLightFrames && same_place) {
    light = it->second;
    return true;
  }
  if (own_none) *own_none = it == g_model_lights.end();  // else its own was too old
  if (!at || !REXCVAR_GET(cod3_debug_ngl_side_light_by_place) || g_kept_light_cells.empty() ||
      !std::isfinite(at[0] + at[1] + at[2]) || std::fabs(at[0]) > 1.0e6f || std::fabs(at[1]) > 1.0e6f ||
      std::fabs(at[2]) > 1.0e6f)
    return false;
  const uint32_t set = ObjectLightSet(base, object);
  const GameModelLight* best = nullptr;
  bool best_same_set = false;
  float best_distance = kKeptLightNear * kKeptLightNear;
  const int64_t cx = KeptLightCellOf(at[0]), cy = KeptLightCellOf(at[1]), cz = KeptLightCellOf(at[2]);
  for (int64_t x = cx - 1; x <= cx + 1; ++x) {
    for (int64_t y = cy - 1; y <= cy + 1; ++y) {
      for (int64_t z = cz - 1; z <= cz + 1; ++z) {
        const auto cell = g_kept_light_cells.find(KeptLightCellKey(x, y, z));
        if (cell == g_kept_light_cells.end()) continue;
        for (int k = 0; k < kKeptLightsPerCell; ++k) {
          const GameModelLight& kept = cell->second.lights[k];
          if (!cell->second.objects[k] || !kept.placed || frame > kept.frame + kModelLightFrames) continue;
          float distance = 0.0f;
          for (int i = 0; i < 3; ++i) distance += (kept.at[i] - at[i]) * (kept.at[i] - at[i]);
          if (!(distance <= kKeptLightNear * kKeptLightNear)) continue;
          const bool same_set = set != 0 && kept.set == set;
          if (best && (best_same_set && !same_set)) continue;
          if (best && best_same_set == same_set && distance >= best_distance) continue;
          best = &kept;
          best_same_set = same_set;
          best_distance = distance;
        }
      }
    }
  }
  if (!best) return false;
  light = *best;
  if (by_place) *by_place = true;
  return true;
}

bool GameLightFor(uint8_t* base, uint32_t object, uint64_t frame, ngl_vk_mesh_draw& d, bool* by_place,
                  bool* own_none, uint64_t* kept_frame = nullptr, GameModelLight* taken = nullptr) {
  GameModelLight light;
  if (kept_frame) *kept_frame = 0;
  if (taken) *taken = GameModelLight{};
  const float at[3] = {d.world[12], d.world[13], d.world[14]};
  if (!FindKeptLight(base, object, frame, at, light, by_place, own_none)) return false;
  if (kept_frame) *kept_frame = light.frame;
  if (taken) *taken = light;
  double r[3][3];
  for (int i = 0; i < 3; ++i) {
    const double length = std::sqrt(double{d.world[4 * i]} * d.world[4 * i] +
                                    double{d.world[4 * i + 1]} * d.world[4 * i + 1] +
                                    double{d.world[4 * i + 2]} * d.world[4 * i + 2]);
    if (!(length > 1e-6)) return false;
    for (int j = 0; j < 3; ++j) r[i][j] = d.world[4 * i + j] / length;
  }
  for (int c = 0; c < 3; ++c) {  // red, green, blue
    const float(&w)[9][3] = light.rows;
    const double m[3][3] = {{w[0][c], 0.5 * w[3][c], 0.5 * w[5][c]},
                            {0.5 * w[3][c], w[1][c], 0.5 * w[4][c]},
                            {0.5 * w[5][c], 0.5 * w[4][c], w[2][c]}};
    const double b[3] = {w[6][c], w[7][c], w[8][c]};
    double turned[3][3] = {}, along[3] = {};
    for (int i = 0; i < 3; ++i) {
      for (int l = 0; l < 3; ++l) {
        for (int j = 0; j < 3; ++j) {
          for (int k = 0; k < 3; ++k) turned[i][l] += r[i][j] * m[j][k] * r[l][k];
        }
      }
      for (int j = 0; j < 3; ++j) along[i] += r[i][j] * b[j];
    }
    d.irradiance[0][c] = static_cast<float>(turned[0][0]);
    d.irradiance[1][c] = static_cast<float>(turned[1][1]);
    d.irradiance[2][c] = static_cast<float>(turned[2][2]);
    d.irradiance[3][c] = static_cast<float>(2.0 * turned[0][1]);
    d.irradiance[4][c] = static_cast<float>(2.0 * turned[1][2]);
    d.irradiance[5][c] = static_cast<float>(2.0 * turned[2][0]);
    for (int i = 0; i < 3; ++i) d.irradiance[6 + i][c] = static_cast<float>(along[i]);
  }
  d.irradiance_rows = 9;
  d.extra_light_count = 0;
  d.light_saturate = 0;  // the per-pixel shaders don't hold the light to 1
  d.point_light_count = 0;
  if (light.point[3] > 0.0f) {  // the models' shaders take the nearest one (LIGHT_POSITION1)
    std::memcpy(d.point_light[0], light.point, sizeof light.point);
    d.point_light[0][7] = 0.0f;
    d.point_light_count = 1;
  }
  return true;
}

// [rexglue-native] 2026-11-10 rn43a: the object whose native draw is being built (DrawnScope, in SolidDraws), for
// DrawnValue. At its first value read, the light kept for it is looked up as GameLightFor does (its own, else by
// place), and the values come from what was kept with that light (for the object it was kept for). used and late
// count the values taken as drawn and those read from the block as it is now (no value kept), for the census.
// rn43c: when that object kept none for this block, the values kept lately for this block nearest where the model
// stands (within kKeptLightNear, g_drawn_cells); by_place counts the values taken so.
struct DrawnLookup {
  uint32_t object = 0;
  uint64_t frame = 0;
  bool looked = false;
  uint32_t source = 0;  // the object the light found was kept for (0: none found)
  float at[3] = {};     // rn43c: where the model stands, when placed
  bool placed = false;
  int used = 0, late = 0, by_place = 0;
};
thread_local DrawnLookup t_drawn;
class DrawnScope {
 public:
  DrawnScope(uint32_t object, uint64_t frame) {
    t_drawn = DrawnLookup{};
    t_drawn.object = object;
    t_drawn.frame = frame;
  }
  ~DrawnScope() { t_drawn = DrawnLookup{}; }
  DrawnScope(const DrawnScope&) = delete;
  DrawnScope& operator=(const DrawnScope&) = delete;
};
bool DrawnValue(uint8_t* base, uint32_t block, uint32_t offset, float* into, uint32_t floats) {
  if (!t_drawn.object || !block || floats > 4 || !REXCVAR_GET(cod3_debug_ngl_side_drawn_values)) return false;
  if (!t_drawn.looked) {
    t_drawn.looked = true;
    t_drawn.placed = ObjectPlace(base, t_drawn.object, t_drawn.at);  // (not placed: its own light only)
    GameModelLight light;
    if (FindKeptLight(base, t_drawn.object, t_drawn.frame, t_drawn.placed ? t_drawn.at : nullptr, light, nullptr,
                      nullptr)) {
      t_drawn.source = light.object;
    }
  }
  float kept[4] = {};
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(g_model_light_mutex);
    // the offset, noted for the block, so the lit draws from now on keep it
    auto noted = g_drawn_offsets.find(block);
    if (noted == g_drawn_offsets.end()) {
      if (g_drawn_offsets.size() >= kMaxDrawnBlocks) g_drawn_offsets.clear();
      noted = g_drawn_offsets.emplace(block, DrawnOffsets{}).first;
    }
    DrawnOffsets& offsets = noted->second;
    bool known = false;
    for (uint32_t k = 0; k < offsets.count; ++k) known = known || offsets.offsets[k] == offset;
    if (!known && offsets.count < kDrawnOffsets) offsets.offsets[offsets.count++] = offset;
    const auto values = t_drawn.source ? g_drawn_values.find(DrawnValuesKey(t_drawn.source, block))
                                       : g_drawn_values.end();
    const DrawnValues* taken = nullptr;
    if (values != g_drawn_values.end() && t_drawn.frame <= values->second.frame + kModelLightFrames) {
      taken = &values->second;
    } else if (t_drawn.placed && REXCVAR_GET(cod3_debug_ngl_side_light_by_place)) {  // rn43c: by place
      float best_distance = kKeptLightNear * kKeptLightNear;
      const int64_t cx = KeptLightCellOf(t_drawn.at[0]), cy = KeptLightCellOf(t_drawn.at[1]),
                    cz = KeptLightCellOf(t_drawn.at[2]);
      for (int64_t x = cx - 1; x <= cx + 1; ++x) {
        for (int64_t y = cy - 1; y <= cy + 1; ++y) {
          for (int64_t z = cz - 1; z <= cz + 1; ++z) {
            const auto cell = g_drawn_cells.find(DrawnCellKey(block, x, y, z));
            if (cell == g_drawn_cells.end()) continue;
            for (int k = 0; k < kKeptLightsPerCell; ++k) {
              const DrawnValues& kept_here = cell->second.values[k];
              if (!cell->second.objects[k] || kept_here.block != block || !kept_here.placed ||
                  t_drawn.frame > kept_here.frame + kModelLightFrames)
                continue;
              float distance = 0.0f;
              for (int i = 0; i < 3; ++i) {
                const float apart = kept_here.at[i] - t_drawn.at[i];
                distance += apart * apart;
              }
              if (!(distance <= best_distance)) continue;
              taken = &kept_here;
              best_distance = distance;
            }
          }
        }
      }
      if (taken) ++t_drawn.by_place;
    }
    for (uint32_t k = 0; taken && k < taken->count && !found; ++k) {
      if (taken->offsets[k] != offset) continue;
      found = true;
      for (uint32_t i = 0; i < 4; ++i) kept[i] = taken->values[k][i];
    }
  }
  for (uint32_t i = 0; found && i < floats; ++i) found = std::isfinite(kept[i]);
  if (!found) {
    ++t_drawn.late;
    return false;
  }
  for (uint32_t i = 0; i < floats; ++i) into[i] = kept[i];
  ++t_drawn.used;
  return true;
}

void LightWith(ngl_vk_mesh_draw& d, const Sun& sun) {
  if (!sun.ok) return;
  d.lit = 1;
  std::memcpy(d.ambient, sun.ambient, sizeof d.ambient);
  std::memcpy(d.light_direction, sun.direction, sizeof d.light_direction);
  std::memcpy(d.light_color, sun.color, sizeof d.light_color);
  d.extra_light_count = sun.more;
  std::memcpy(d.extra_light_direction, sun.more_direction, sizeof d.extra_light_direction);
  std::memcpy(d.extra_light_color, sun.more_color, sizeof d.extra_light_color);
  d.light_saturate = 1;  // the game's vertex shaders saturate the lit color (shader_C4130C72D4721102's add_sat)
}

// The scene's fog, as the game sets it for each view: R_SetupFog (0x82417E08, called by 0x82430210) passes its
// distances and amounts to nglSetFog (0x8212AE18) and its color to nglSetFogColor (0x8212B5C8), Big Red One's
// 0x5E4D0 and 0x5EB60 with the scene's fields 0x50 further on. +0x3D8 the distance the fog starts at (never nearer
// than the camera's near plane, +0x414), +0x3DC the distance it ends at, +0x3E0 its amount before the start, +0x3E4
// its amount at the end; +0x3F0 (16-byte aligned) its color, red, green and blue from 0 to 1, then 1.
constexpr uint32_t kSceneFog = 0x3D8;
constexpr uint32_t kSceneFogColor = 0x3F0;

uint32_t UnitByte(float f) {
  const float clamped = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
  return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
}

void FogWith(uint8_t* base, uint32_t scene, ngl_vk_mesh_draw& d) {
  if (!Readable(scene + kSceneFogColor + 12)) return;
  float f[7];
  for (uint32_t i = 0; i < 4; ++i) f[i] = g::ReadF32(base, scene + kSceneFog + 4 * i);
  const uint32_t color = (scene + kSceneFogColor) & ~0xFu;
  for (uint32_t i = 0; i < 3; ++i) f[4 + i] = g::ReadF32(base, color + 4 * i);
  for (float v : f) {
    if (!std::isfinite(v)) return;
  }
  static std::unordered_set<uint32_t> shown;
  if (shown.size() < kShownScenes && shown.insert(scene).second) {
    REXLOG_INFO("cod3: ngl side: scene {:08X} fog: from {:.1f} to {:.1f}, amount {:.3f} to {:.3f}, color {:.3f} {:.3f} "
                "{:.3f}", scene, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
  }
  if (!(f[1] > f[0])) return;  // no fog
  d.fog[0] = f[0];
  d.fog[1] = f[1];
  d.fog[2] = f[3];
  d.fog_color = UnitByte(f[4]) | UnitByte(f[5]) << 8 | UnitByte(f[6]) << 16 | UnitByte(f[2]) << 24;
}
#else
// rn43a: without ngl_vk no native draw is built, so no value is taken as drawn
bool DrawnValue(uint8_t*, uint32_t, uint32_t, float*, uint32_t) { return false; }
#endif

// The material's first texture, looking in its first 0x60 bytes and one pointer deep; 0 for none.
uint32_t MaterialTexture(uint8_t* base, uint32_t material) {
  if (!Pointer(material)) return 0;
  for (uint32_t at = 0; at < 0x60; at += 4) {
    const uint32_t word = g::ReadU32(base, material + at);
    if (IsTexture(base, word)) return word;
  }
  for (uint32_t at = 0; at < 0x60; at += 4) {
    const uint32_t inner = g::ReadU32(base, material + at);
    if (!Physical(inner) || (inner & 3) || inner == material) continue;
    for (uint32_t k = 0; k < 0x40; k += 4) {
      const uint32_t word = g::ReadU32(base, inner + k);
      if (IsTexture(base, word)) return word;
    }
  }
  return 0;
}

// side90: the second texture a material holds in its first 0x60 bytes (a cdWorld's lightmap), 0 for none.
uint32_t SecondTexture(uint8_t* base, uint32_t material) {
  if (!Pointer(material)) return 0;
  uint32_t first = 0;
  for (uint32_t at = 0; at < 0x60; at += 4) {
    const uint32_t word = g::ReadU32(base, material + at);
    if (!IsTexture(base, word)) continue;
    if (!first) {
      first = word;
    } else if (word != first) {
      return word;
    }
  }
  return 0;
}

#if defined(COD3_NGL_VK)
std::filesystem::path ScreenshotFolder();
void SavePpm(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height);
#endif

// A texture saved as a picture (userdata/logs/screenshots/ngl-side-texture-<address>.ppm), to see what it holds.
void SaveTexture(uint8_t* base, uint32_t texture) {
#if defined(COD3_NGL_VK)
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!IsTexture(base, texture) ||
      !Decode(base, g::ReadU32(base, texture + kTextureDirect3D), bgra, width, height, why)) {
    return;
  }
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  char name[64];
  std::snprintf(name, sizeof name, "ngl-side-texture-%08X.ppm", texture);
  SavePpm(ScreenshotFolder() / name, bgra.data(), width, height);
#else
  (void)base;
  (void)texture;
#endif
}

// A generic surface in full, for the first few: the surface's buffers, its format, its first vertex bytes and
// indices, so the guesses at the top can be checked.
void ShowSurface(uint8_t* base, uint32_t node, int& shown) {
  if (shown >= kShownSurfaces) return;
  ++shown;
  const uint32_t surface = g::ReadU32(base, node + kNodeSurface);
  if (!Pointer(surface)) return;
  REXLOG_INFO("cod3: ngl side: surface {:08X} (node {:08X}, object {:08X}, material {:08X})", surface, node,
              g::ReadU32(base, node + kNodeObject), g::ReadU32(base, node + kNodeMaterial));
  for (uint32_t row = 0; row < 0x80; row += 0x20) {
    REXLOG_INFO("cod3: ngl side: surface {:08X} +{:02X}: {}", surface, row, Words(base, surface + row, 8));
  }
  for (uint32_t k = 0; k < 2; ++k) {
    const uint32_t buffer = g::ReadU32(base, surface + kSurfaceBuffers + 0x10 * k);
    if (!Pointer(buffer)) continue;
    REXLOG_INFO("cod3: ngl side: surface {:08X} buffer {}: {:08X}: {}", surface, k, buffer, Words(base, buffer, 8));
    const uint32_t f0 = g::ReadU32(base, buffer + kBufferFetch);
    const uint32_t data = GuestAddress((f0 & 3) == 3 ? f0 & ~3u : f0);
    if (Physical(data)) {
      for (uint32_t row = 0; row < 0x60; row += 0x20) {
        REXLOG_INFO("cod3: ngl side: surface {:08X} buffer {} data {:08X} +{:02X}: {}", surface, k, data, row,
                    Words(base, data + row, 8));
      }
    }
  }
  const uint32_t material = g::ReadU32(base, node + kNodeMaterial);
  const uint32_t texture = MaterialTexture(base, material);
  REXLOG_INFO("cod3: ngl side: surface {:08X} material {:08X}: texture {:08X} ({}x{}); all: {}", surface, material,
              texture, Pointer(texture) ? g::ReadU32(base, texture + kTextureWidth) : 0,
              Pointer(texture) ? g::ReadU32(base, texture + kTextureHeight) : 0,
              Pointer(material) ? MaterialTextures(base, material) : std::string("-"));
  if (Pointer(material)) {
    for (uint32_t at : {0x28u, 0x30u, 0x54u}) {
      const uint32_t inner = g::ReadU32(base, material + at);
      if (!Physical(inner) || (inner & 3)) continue;
      for (uint32_t row = 0; row < 0x60; row += 0x20) {
        REXLOG_INFO("cod3: ngl side: material {:08X} +{:02X} -> {:08X} +{:02X}: {}", material, at, inner, row,
                    Words(base, inner + row, 8));
      }
    }
  }
  SaveTexture(base, texture);
  const Samplers samplers = MaterialSamplers(base, material, false);  // (the listing: as for a solid)
  SaveTexture(base, samplers.color);
  SaveTexture(base, samplers.lightmap);
  REXLOG_INFO("cod3: ngl side: surface {:08X}: color map {:08X}, lightmap {:08X}", surface, samplers.color,
              samplers.lightmap);
  const uint32_t list = Pointer(material) ? g::ReadU32(base, material + kMaterialSamplers) : 0;
  for (uint32_t k = 0; Readable(list) && k < kMaxSamplers; ++k) {
    const uint32_t parameter = g::ReadU32(base, list + kSamplerFirst + kSamplerBytes * k);
    if (!Physical(parameter) || (parameter & 3)) break;
    std::string name;
    for (uint32_t at = 0; at < 0x20 && name.empty(); at += 4) {  // a name the parameter points to, if any
      const uint32_t text = g::ReadU32(base, parameter + at);
      if (!Physical(text) && !(text >= 0x82000000u && text < kImageEnd)) continue;
      const char* c = reinterpret_cast<const char*>(base + text);
      size_t n = 0;
      while (n < 32 && c[n] >= 32 && c[n] < 127) ++n;
      if (n >= 4 && c[n] == 0) name.assign(c, n);
    }
    REXLOG_INFO("cod3: ngl side: sampler {} parameter {:08X} \"{}\": {}", k, parameter, name,
                Words(base, parameter, 8));
  }
  const uint32_t object = g::ReadU32(base, node + kNodeObject);
  if (Pointer(object)) {
    REXLOG_INFO("cod3: ngl side: object {:08X} textures: {}", object, MaterialTextures(base, object, 0xC0));
  }
  const uint32_t format = g::ReadU32(base, surface + kSurfaceFormat);
  if (Pointer(format)) {
    REXLOG_INFO("cod3: ngl side: surface {:08X} format {:08X}: {} / {}", surface, format, Words(base, format, 8),
                Words(base, format + 0x20, 8));
  }
  const uint32_t elements = g::ReadU32(base, surface + kSurfaceStride + 4);
  if (Pointer(elements)) {
    REXLOG_INFO("cod3: ngl side: surface {:08X} +74 -> {:08X}: {} / {}", surface, elements, Words(base, elements, 8),
                Words(base, elements + 0x20, 8));
  }
}

void ShowSurface(uint8_t* base, uint32_t node) {
  static int shown = 0;
  ShowSurface(base, node, shown);
}

// Step 4's first look: the first node of each other kind (the 3D ones), once: its vtable's first eight entries (Render
// is +8), its first 0x60 bytes, and its scene's +0x100..+0x1FF (where Big Red One keeps the projection and view).
// A tree or grass node in depth, for the first few (step 4's next part): the node, then every block its words point to
// (0x40 bytes), and one level further for the Direct3D buffers among them (common word 0x00200001 vertex,
// 0x20200002 index), to find where SpeedTree keeps its geometry.
// The grass node, once (side55-57): +0x0C the grass object G, +0x10 the patch set. G's first word is the grass
// manager M that 0x823F6DF0 works with: M+0x00 the NGL material N, +0x04 and +0x28 the shared vertex and index
// buffers (+0x24 7000 vertices, +0x2C 8000 indices), +0x4C the spacing, +0x50 and +0x54 the samplers, +0x58 to +0xC4
// N's other parameters (kind at +0x10: 3 a float, 4 four, 0xB a sampler; value at +0x14), +0x194..+0x19C. N: +0x30
// NGL's parameter list (+0x04 name, +0x0C next, +0x10 kind, +0x14 value), +0x1C its value block, +0x0C's +0x0C its
// pass, whose compiled constant uploads (+0xDC) are lists at +0x80, +0xC0, +0x100 and +0x140 of {slot, offset, a
// SET_CONSTANT packet's 32 bytes} (0x82154DC0: the value is at the slot's base + offset; slot 1 is N+0x1C's block,
// slot 2 NGL's globals at 0x8290E9AC). The set: +0x04 the list of patches (count at +0x0E), each naming its cell
// (+0x0C, an index into the set's +0x00 list of {kind, data} pairs).
void ShowGrass(uint8_t* base, uint32_t node) {
  static bool shown = false;
  if (shown) return;
  shown = true;
  auto readable = [](uint32_t a) { return Readable(a) || (!(a & 3) && a >= 0x40000000u && a < 0x80000000u); };
  auto floats = [base](uint32_t at, int count) {
    std::string text;
    char item[24];
    for (int k = 0; k < count; ++k) {
      std::snprintf(item, sizeof item, "%s%.4g", k ? " " : "", g::ReadF32(base, at + 4u * static_cast<uint32_t>(k)));
      text += item;
    }
    return text;
  };
  const uint32_t object = g::ReadU32(base, node + 0x0C), set = g::ReadU32(base, node + 0x10);
  const uint32_t material = readable(object) ? g::ReadU32(base, object) : 0;
  REXLOG_INFO("cod3: ngl side: grass node {:08X}: {}; object {:08X}, material {:08X}, patch set {:08X}", node,
              Words(base, node, 8), object, material, set);
  if (readable(material)) {
    for (uint32_t row = 0; row < 0x1C0; row += 0x20) {
      REXLOG_INFO("cod3: ngl side: grass material +{:03X}: {}", row, Words(base, material + row, 8));
    }
    for (uint32_t at = 0x50; at <= 0xC4; at += 4) {
      const uint32_t parameter = g::ReadU32(base, material + at);
      if (!readable(parameter)) continue;
      const uint32_t value = g::ReadU32(base, parameter + 0x14);
      REXLOG_INFO("cod3: ngl side: grass material +{:03X} -> parameter {:08X}: {}; value {:08X}: {} ({})", at,
                  parameter, Words(base, parameter, 8), value, readable(value) ? Words(base, value, 4) : "-",
                  readable(value) ? floats(value, 4) : "-");
    }
    const uint32_t spacing = g::ReadU32(base, material + 0x4C);
    if (readable(spacing)) REXLOG_INFO("cod3: ngl side: grass spacing {:08X}: {}", spacing, floats(spacing, 8));
    REXLOG_INFO("cod3: ngl side: grass material +190: {}", floats(material + 0x190, 4));
    const uint32_t ngl = g::ReadU32(base, material);
    if (!readable(ngl)) return;
    REXLOG_INFO("cod3: ngl side: grass NGL material {:08X}: {} / {}", ngl, Words(base, ngl, 8),
                Words(base, ngl + 0x20, 8));
    int listed = 0;
    for (uint32_t p = g::ReadU32(base, ngl + 0x30); readable(p) && listed < 40;
         p = g::ReadU32(base, p + 0x0C), ++listed) {
      const uint32_t name = g::ReadU32(base, p + 0x04), value = g::ReadU32(base, p + 0x14);
      const char* text = readable(name) ? reinterpret_cast<const char*>(base + name + 4) : "";
      REXLOG_INFO("cod3: ngl side: grass parameter {:08X} \"{}\": kind {:X}; {}; value {:08X}: {}", p,
                  std::string(text, strnlen(text, 48)), g::ReadU32(base, p + 0x10), Words(base, p, 8), value,
                  readable(value) ? floats(value, 4) : "-");
    }
    const uint32_t holder = g::ReadU32(base, ngl + 0x0C);
    const uint32_t pass = readable(holder) ? g::ReadU32(base, holder + 0x0C) : 0;
    const uint32_t compiled = readable(pass) ? g::ReadU32(base, pass + 0xDC) : 0;
    REXLOG_INFO("cod3: ngl side: grass pass {:08X} (+0xD0 {:X}, +0x18 {:X}, +0xA4 {:X}), compiled {:08X}", pass,
                readable(pass) ? g::ReadU32(base, pass + 0xD0) : 0, readable(pass) ? g::ReadU32(base, pass + 0x18) : 0,
                readable(pass) ? g::ReadU32(base, pass + 0xA4) : 0, compiled);
    if (readable(compiled)) {
      for (uint32_t row = 0; row < 0x180; row += 0x20) {
        REXLOG_INFO("cod3: ngl side: grass compiled +{:03X}: {}", row, Words(base, compiled + row, 8));
      }
    }
    const uint32_t bases[8] = {0, g::ReadU32(base, ngl + 0x1C), g::ReadU32(base, kCurrentScene), kPassBase3,
                               kPassBase4, 0, kPassBase6, g::ReadU32(base, kPassBase7Pointer)};
    for (uint32_t list = 0x80; readable(compiled) && list <= 0x140; list += 0x40) {
      const uint32_t at = compiled + list;
      const uint32_t vectors = g::ReadU32(base, at + 0x34), entries = g::ReadU32(base, at + 0x38);
      REXLOG_INFO("cod3: ngl side: grass constants +{:03X}: {} / {} ({} vectors)", list, Words(base, at, 8),
                  Words(base, at + 0x20, 8), vectors);
      // The float constants (0x82155EC0): a SET_CONSTANT packet's header at +0x10 (its first register x 4 at +0x1C),
      // then one {byte offset into the bases, offset} pair a vector at +0x20 (+0x08 the number of vectors).
      const uint32_t count = g::ReadU32(base, at + 0x08), rows = g::ReadU32(base, at + 0x20);
      const uint32_t first = (g::ReadU32(base, at + 0x1C) & 0x7FF) / 4;
      for (uint32_t k = 0; k < count && k < 96 && readable(rows); ++k) {
        const uint32_t slot = g::ReadU32(base, rows + 8 * k) / 4, offset = g::ReadU32(base, rows + 8 * k + 4);
        const uint32_t value = ((slot < 8 ? bases[slot] : 0) + offset) & ~15u;
        REXLOG_INFO("cod3: ngl side: grass c{}: slot {} +{:X} -> {:08X}: {}", first + k, slot, offset, value,
                    readable(value) ? floats(value, 4) : "-");
      }
      for (uint32_t k = 0; k < vectors && k < 48 && readable(entries); ++k) {
        const uint32_t e = entries + 48 * k, slot = g::ReadU32(base, e), offset = g::ReadU32(base, e + 4);
        const uint32_t value = (slot < 8 ? bases[slot] : 0) + offset;
        REXLOG_INFO("cod3: ngl side: grass constant {}: slot {} +{:X}; packet {} / {}; value {:08X}: {}", k, slot,
                    offset, Words(base, e + 16, 4), Words(base, e + 32, 4), value,
                    readable(value & ~15u) ? floats(value & ~15u, 8) : "-");
      }
    }
  }
  if (!readable(set)) return;
  REXLOG_INFO("cod3: ngl side: grass patch set: {}", Words(base, set, 8));
  const uint32_t cells = g::ReadU32(base, set), list = g::ReadU32(base, set + 4);
  const uint32_t count = g::ReadU32(base, set + 0x0C) & 0xFFFF;
  for (uint32_t k = 0; k < count && k < 4 && readable(list); ++k) {
    const uint32_t patch = g::ReadU32(base, list + 4 * k);
    if (!readable(patch)) continue;
    const uint32_t cell = cells + 8 * (g::ReadU32(base, patch + 0x0C) >> 16);
    const uint32_t kind = g::ReadU32(base, cell), data = g::ReadU32(base, cell + 4);
    const uint32_t descriptor = readable(kind) ? g::ReadU32(base, kind) : 0;
    const uint32_t first = g::ReadU32(base, patch);
    REXLOG_INFO("cod3: ngl side: grass patch {} of {} ({:08X}): {} ({}), +0 -> {}; cell kind {:08X} -> {:08X}: {} "
                "({}); data {:08X}: {}",
                k, count, patch, Words(base, patch, 4), floats(patch, 4), readable(first) ? Words(base, first, 2) : "-",
                kind, descriptor, readable(descriptor) ? Words(base, descriptor, 8) : "-",
                readable(descriptor) ? floats(descriptor, 8) : "-", data, readable(data) ? Words(base, data, 8) : "-");
  }
}

void ShowTree(uint8_t* base, uint32_t node) {
  static int shown = 0;
  if (shown >= 3) return;
  ++shown;
  // the node's record (+0x0C: kind, two words, the tree) and the tree in depth
  const uint32_t record = g::ReadU32(base, node + 0x0C);
  const uint32_t tree = Readable(record) ? g::ReadU32(base, record + 0x0C) : 0;
  if (Readable(tree)) {
    for (uint32_t row = 0; row < 0x100; row += 0x20) {
      REXLOG_INFO("cod3: ngl side: tree {:08X} +{:02X}: {}", tree, row, Words(base, tree + row, 8));
    }
    int followed = 0;
    for (uint32_t at = 0; at < 0x100 && followed < 12; at += 4) {
      const uint32_t inner = g::ReadU32(base, tree + at);
      if (!Readable(inner) || inner == tree) continue;
      ++followed;
      REXLOG_INFO("cod3: ngl side: tree {:08X} +{:02X} -> {:08X}: {} / {}", tree, at, inner, Words(base, inner, 8),
                  Words(base, inner + 0x20, 8));
    }
    for (uint32_t row = 0x100; row < 0x400; row += 0x20) {
      REXLOG_INFO("cod3: ngl side: tree {:08X} +{:03X}: {}", tree, row, Words(base, tree + row, 8));
    }
    // What the branch and frond draws use (sub_823F5C80, sub_823F5868 draw through sub_820C7818): a vertex buffer,
    // index buffers and strip lengths by level of detail, the vertex count, and a vertex format kept in a global
    // ({0x74, 0x4C or 0x6C, ?, declaration}; side23). The buffers, their D3D headers and the declarations sit in the
    // heap's virtual pages (0x4xxxxxxx), followed here only where the game's draw follows them.
    struct Part {
      const char* name;
      uint32_t buffer, index_buffers, lengths, vertices, format;
    };
    const Part parts[] = {{"branches", 0x1C8, 0x1CC, 0x1D0, 0x54, kBranchFormat},
                          {"fronds", 0x1D4, 0x1D8, 0x1DC, 0xAC, kFrondFormat},
                          {"leaves' format", 0, 0, 0, 0, kLeafFormat}};
    auto heap = [](uint32_t a) { return !(a & 3) && a >= 0x40000000u && a < 0x80000000u; };
    for (const Part& part : parts) {
      const uint32_t declaration = g::ReadU32(base, part.format + 8);
      REXLOG_INFO("cod3: ngl side: tree {:08X} {}: format {} -> {} / {} / {} / {}", tree, part.name,
                  Words(base, part.format, 3), heap(declaration) ? Words(base, declaration, 8) : std::string("-"),
                  heap(declaration) ? Words(base, declaration + 0x20, 8) : std::string("-"),
                  heap(declaration) ? Words(base, declaration + 0x40, 8) : std::string("-"),
                  heap(declaration) ? Words(base, declaration + 0x60, 8) : std::string("-"));
      if (!part.buffer) continue;
      const uint32_t buffer = g::ReadU32(base, tree + part.buffer);
      const uint32_t index_buffers = g::ReadU32(base, tree + part.index_buffers);
      REXLOG_INFO("cod3: ngl side: tree {:08X} {}: buffer {:08X}: {}", tree, part.name, buffer,
                  heap(buffer) ? Words(base, buffer, 12) : std::string("-"));
      if (heap(buffer)) {
        const uint32_t data = GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & ~3u);
        if (Physical(data)) {
          REXLOG_INFO("cod3: ngl side: tree {:08X} {} first vertices: {} / {} / {} / {}", tree, part.name,
                      Words(base, data, 8), Words(base, data + 0x20, 8), Words(base, data + 0x40, 8),
                      Words(base, data + 0x60, 8));
        }
      }
      const uint32_t first = Readable(index_buffers) ? g::ReadU32(base, index_buffers) : 0;
      if (heap(first)) {
        const uint32_t data = GuestAddress(g::ReadU32(base, first + 0x18));
        REXLOG_INFO("cod3: ngl side: tree {:08X} {} index buffer 0 {:08X}: {}; first indices {} / {}", tree,
                    part.name, first, Words(base, first, 12), Physical(data) ? Words(base, data, 8) : std::string("-"),
                    Physical(data) ? Words(base, data + 0x20, 8) : std::string("-"));
      }
    }
    for (const uint32_t at : {0x2B0u, 0x2B8u, 0x2CCu, 0x2E8u}) {  // the textures the draws set
      const uint32_t texture = g::ReadU32(base, tree + at);
      REXLOG_INFO("cod3: ngl side: tree {:08X} +{:03X} texture {:08X}: {}", tree, at, texture,
                  Readable(texture) || heap(texture) ? Words(base, texture, 12) : std::string("-"));
    }
    // The leaves (sub_823F5028): a level's record (72 bytes, a list at tree +0xF8: +4 its vertex buffer's number in
    // the list at tree +0x1E0, +8 its leaf count) drawn as a triangle list of six vertices a leaf (sub_820C78D0),
    // format 0x82A1B8DC (stride 0x6C).
    const uint32_t levels = g::ReadU32(base, tree + 0xF8), buffers = g::ReadU32(base, tree + 0x1E0);
    for (uint32_t level = 0; level < 2 && Readable(levels); ++level) {
      const uint32_t record = levels + 72 * level;
      REXLOG_INFO("cod3: ngl side: tree {:08X} leaves level {}: {} / {} / {}", tree, level, Words(base, record, 8),
                  Words(base, record + 0x20, 8), Words(base, record + 0x40, 2));
    }
    const uint32_t buffer = Readable(buffers) ? g::ReadU32(base, buffers) : 0;
    REXLOG_INFO("cod3: ngl side: tree {:08X} leaves: buffers {:08X} ({}), buffer 0 {:08X} ({})", tree, buffers,
                Readable(buffers) ? Words(base, buffers, 4) : std::string("-"), buffer,
                heap(buffer) ? Words(base, buffer, 8) : std::string("-"));
    if (heap(buffer)) {
      const uint32_t data = GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & ~3u);
      for (uint32_t v = 0; v < 6 && Physical(data); ++v) {  // the first leaf's six vertices
        REXLOG_INFO("cod3: ngl side: tree {:08X} leaf vertex {}: {} / {} / {} / {}", tree, v,
                    Words(base, data + 0x6C * v, 8), Words(base, data + 0x6C * v + 0x20, 8),
                    Words(base, data + 0x6C * v + 0x40, 8), Words(base, data + 0x6C * v + 0x60, 3));
      }
    }
  }
  for (uint32_t row = 0; row < 0x80; row += 0x20) {
    REXLOG_INFO("cod3: ngl side: tree node {:08X} +{:02X}: {}", node, row, Words(base, node + row, 8));
  }
  for (uint32_t at = 4; at < 0x80; at += 4) {
    const uint32_t block = g::ReadU32(base, node + at);
    if (!Readable(block) || block == node) continue;
    REXLOG_INFO("cod3: ngl side: tree node {:08X} +{:02X} -> {:08X}: {} / {}", node, at, block, Words(base, block, 8),
                Words(base, block + 0x20, 8));
    for (uint32_t k = 0; k < 0x40; k += 4) {
      const uint32_t inner = g::ReadU32(base, block + k);
      if (!Readable(inner)) continue;
      const uint32_t common = g::ReadU32(base, inner);
      if (common == 0x00200001u || common == 0x20200002u) {
        REXLOG_INFO("cod3: ngl side: tree node {:08X} +{:02X} -> +{:02X}: {} buffer {:08X}: {}", node, at, k,
                    common == 0x00200001u ? "vertex" : "index", inner, Words(base, inner, 8));
      }
    }
  }
}

void ShowNode(uint8_t* base, uint32_t scene, uint32_t node, uint32_t vtable) {
  static std::unordered_set<uint32_t> shown;
  static bool scene_shown = false;
  if (shown.size() >= 8 || !shown.insert(vtable).second) return;
  auto words = [base](uint32_t at, uint32_t count) {
    std::string text;
    char item[12];
    for (uint32_t k = 0; k < count; ++k) {
      std::snprintf(item, sizeof item, "%s%08X", k ? " " : "", g::ReadU32(base, at + 4 * k));
      text += item;
    }
    return text;
  };
  REXLOG_INFO("cod3: ngl side: node kind {:08X}: vtable {}", vtable, Pointer(vtable) ? words(vtable, 8) : "?");
  for (uint32_t row = 0; row < 0x60; row += 0x20) {
    REXLOG_INFO("cod3: ngl side: node {:08X} (kind {:08X}) +{:02X}: {}", node, vtable, row, words(node + row, 8));
  }
  if (!scene_shown && Pointer(scene)) {
    scene_shown = true;
    for (uint32_t row = 0x100; row < 0x200; row += 0x20) {
      REXLOG_INFO("cod3: ngl side: scene {:08X} +{:03X}: {}", scene, row, words(scene + row, 8));
    }
  }
  // the generic surface node (Render 0x82143F70): its object, surface and material, and the globals its pass setup
  // (0x821563A8) hands the effect system (0x829118E0, 0x829112A0), to find the transforms and the camera
  if (vtable == kGenericNode) {
    const uint32_t object = g::ReadU32(base, node + 0x0C), surface = g::ReadU32(base, node + 0x10);
    const uint32_t material = g::ReadU32(base, node + 0x14);
    const struct {
      const char* what;
      uint32_t at, bytes;
    } blocks[] = {{"object", object, 0xC0},     {"surface", surface, 0x80},    {"material", material, 0x60},
                  {"pass base 3", kPassBase3, 0x140}, {"pass base 4", kPassBase4, 0x40}};
    for (const auto& b : blocks) {
      if (!Pointer(b.at)) continue;
      for (uint32_t row = 0; row < b.bytes; row += 0x20) {
        REXLOG_INFO("cod3: ngl side: {} {:08X} +{:03X}: {}", b.what, b.at, row, words(b.at + row, 8));
      }
    }
  }
}

// One list's nodes in the order they came (the list is pushed at its head).
std::vector<uint32_t> ListNodes(uint8_t* base, uint32_t head) {
  std::vector<uint32_t> nodes;
  const uint32_t end = g::ReadU32(base, kListEndPointer);
  for (uint32_t n = head; Pointer(n) && n != end && nodes.size() < kMaxNodes; n = g::ReadU32(base, n + kNodeNext)) {
    nodes.push_back(n);
  }
  std::reverse(nodes.begin(), nodes.end());
  return nodes;
}

Quad ReadQuad(uint8_t* base, uint32_t quad) {
  Quad q;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t vertex = quad + i * kQuadVertexBytes;
    q.x[i] = g::ReadF32(base, vertex);
    q.y[i] = g::ReadF32(base, vertex + 4);
    q.u[i] = g::ReadF32(base, vertex + 8);
    q.v[i] = g::ReadF32(base, vertex + 12);
    q.color[i] = g::ReadU32(base, vertex + 16);
  }
  q.texture = g::ReadU32(base, quad + kQuadTexture);
  for (uint32_t k = 0; k < 4; ++k) q.extra[k] = g::ReadU32(base, quad + 0x50 + 4 * k);
  return q;
}

float SaneScale(float scale) { return scale > 0.0f && scale < 100.0f ? scale : 1.0f; }

// A control code's number, as nglBuildTextGlyphs reads it: one byte, the number, one byte.
double CodeNumber(const std::string& s, size_t& i, bool hex) {
  if (i < s.size()) ++i;
  const char* begin = s.c_str() + i;
  char* end = nullptr;
  const double value = hex ? static_cast<double>(std::strtoul(begin, &end, 16)) : std::strtod(begin, &end);
  i += static_cast<size_t>(end - begin);
  if (i < s.size()) ++i;
  return value;
}

// A string node's glyphs as quads, laid out the way nglBuildTextGlyphs does (bro/src/ngl/ngl_font.c, lay_out): byte 1
// starts a color (hex RRGGBBAA), 2 a scale, 3 an x and a y scale; a tab is four spaces, a new line goes down the cell
// height times the line's largest y scale, back to the start.
void TakeString(uint8_t* base, uint32_t scene, uint32_t node, std::vector<Quad>& out, Census& census) {
  Text t;
  t.node = node;
  t.font = g::ReadU32(base, node + kStringFont);
  t.color = g::ReadU32(base, node + kStringColor);
  t.x = g::ReadF32(base, node + kStringX);
  t.y = g::ReadF32(base, node + kStringY);
  t.scale_x = g::ReadF32(base, node + kStringScaleX);
  t.scale_y = g::ReadF32(base, node + kStringScaleY);
  t.glyphs = 0;
  const uint32_t text = g::ReadU32(base, node + kStringText);
  if (text >= 0x10000) {
    const char* p = reinterpret_cast<const char*>(base + text);
    t.text.assign(p, strnlen(p, kMaxText));
  }
  const uint32_t font = t.font;
  const uint32_t glyphs = Pointer(font) ? g::ReadU32(base, font + kFontGlyphs) : 0;
  const int32_t first = Pointer(font) ? static_cast<int32_t>(g::ReadU32(base, font + kFontFirst)) : 0;
  const int32_t count = Pointer(font) ? static_cast<int32_t>(g::ReadU32(base, font + kFontCount)) : 0;
  if (!Pointer(glyphs) || count <= 0 || count > 0x10000) {
    census.texts.push_back(t);
    return;
  }
  const uint32_t texture = g::ReadU32(base, font + kFontTexture);
  const float cell = static_cast<float>(static_cast<int32_t>(g::ReadU32(base, font + kFontCellHeight)));
  const float texture_width = Pointer(texture) ? static_cast<float>(g::ReadU32(base, texture + kTextureWidth)) : 0.0f;
  const float texture_height = Pointer(texture) ? static_cast<float>(g::ReadU32(base, texture + kTextureHeight)) : 0.0f;
  // a glyph: x, y, width, height, x offset, y offset, advance; -1 when the font has none for c
  auto glyph = [&](unsigned char c, int32_t m[7]) -> int32_t {
    const int32_t index = static_cast<int32_t>(c) - first;
    if (index < 0 || index >= count) return -1;
    for (uint32_t k = 0; k < 7; ++k) {
      m[k] = static_cast<int32_t>(g::ReadU32(base, glyphs + static_cast<uint32_t>(index) * kGlyphBytes + 4 * k));
    }
    return index;
  };
  int32_t space[7] = {};
  const float space_advance = glyph(' ', space) >= 0 ? static_cast<float>(space[6]) : 0.0f;
  float scale_x = SaneScale(t.scale_x), scale_y = SaneScale(t.scale_y);
  float x = t.x, y = t.y, line_scale = scale_y;
  uint32_t color = t.color;
  const std::string& s = t.text;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    switch (c) {
      case 1: {
        const uint32_t rgba = static_cast<uint32_t>(CodeNumber(s, i, true));
        color = rgba << 24 | rgba >> 8;
        break;
      }
      case 2:
        scale_x = scale_y = SaneScale(static_cast<float>(CodeNumber(s, i, false)));
        line_scale = std::max(line_scale, scale_y);
        break;
      case 3:
        scale_x = SaneScale(static_cast<float>(CodeNumber(s, i, false)));
        --i;  // the two numbers share the byte between them
        scale_y = SaneScale(static_cast<float>(CodeNumber(s, i, false)));
        line_scale = std::max(line_scale, scale_y);
        break;
      case '\t':
        x += space_advance * scale_x * 4.0f;
        break;
      case '\n':
        y += cell * line_scale;
        x = t.x;
        line_scale = scale_y;
        break;
      case ' ':
        x += space_advance * scale_x;
        break;
      default: {
        int32_t m[7];
        const int32_t index = glyph(c, m);
        if (index < 0) break;
        if (m[2] > 0 && m[3] > 0) {
          float u0, v0, u1, v1;
          if (texture_width > 0.0f && texture_height > 0.0f) {
            u0 = static_cast<float>(m[0]) / texture_width;
            v0 = static_cast<float>(m[1]) / texture_height;
            u1 = u0 + static_cast<float>(m[2]) / texture_width;
            v1 = v0 + static_cast<float>(m[3]) / texture_height;
          } else {
            u0 = v0 = u1 = v1 = 0.0f;
          }
          const float left = static_cast<float>(m[4]) * scale_x + x, top = static_cast<float>(m[5]) * scale_y + y;
          const float right = static_cast<float>(m[2]) * scale_x + left;
          const float bottom = static_cast<float>(m[3]) * scale_y + top;
          Quad q;
          const float xs[4] = {left, right, left, right}, ys[4] = {top, top, bottom, bottom};
          const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
          for (int k = 0; k < 4; ++k) {
            q.x[k] = xs[k];
            q.y[k] = ys[k];
            q.u[k] = us[k];
            q.v[k] = vs[k];
            q.color[k] = color;
          }
          q.texture = texture;
          q.scene = scene;
          out.push_back(q);
          t.glyphs++;
          census.glyphs++;
        }
        x += static_cast<float>(m[6]) * scale_x;
        break;
      }
    }
  }
  census.texts.push_back(t);
}

// side90: which old material class a node's Render is, if any.
int OldClassOf(uint8_t* base, uint32_t vtable) {
  if (!Pointer(vtable) || !Readable(vtable)) return kNotOld;
  const uint32_t render = g::ReadU32(base, vtable + kVtableRender);
  return render == kSimpleDraw ? kOldSimple : render == kWorldDraw ? kOldWorld : kNotOld;
}

void TakeList(uint8_t* base, uint32_t scene, uint32_t head, bool blended, std::vector<Quad>& out, Census& census) {
  std::vector<uint32_t> nodes = ListNodes(base, head);
  if (blended) {
    std::stable_sort(nodes.begin(), nodes.end(), [base](uint32_t a, uint32_t b) {
      return g::ReadF32(base, a + kNodeKey) > g::ReadF32(base, b + kNodeKey);
    });
  }
  for (const uint32_t n : nodes) {
    const uint32_t vtable = g::ReadU32(base, n);
    if (vtable == kQuadNode) {
      out.push_back(ReadQuad(base, n + kNodePayload));
      out.back().scene = scene;
      census.quads++;
    } else if (vtable == kStringNode) {
      census.strings++;
      TakeString(base, scene, n, out, census);
    } else {
      census.other_nodes++;
      census.others[vtable]++;
      ShowNode(base, scene, n, vtable);
      if (vtable == kGenericNode) {
        census.solids.push_back({scene, n});
        ShowSurface(base, n);
      } else if (const int old = OldClassOf(base, vtable)) {
        census.solids.push_back({scene, n, old});
        census.old_nodes++;
      } else if (vtable == kTreeNode) {
        census.trees.push_back({scene, n});
        ShowTree(base, n);
      } else if (kGrassNode && vtable == kGrassNode) {
        census.grass.push_back({scene, n});
        ShowGrass(base, n);
      }
    }
  }
}

void TakeScene(uint8_t* base, uint32_t scene, int depth, std::vector<Quad>& out, Census& census) {
  if (!Pointer(scene) || depth > 16 || census.scenes >= static_cast<int>(kMaxScenes)) return;
  census.scenes++;
  for (uint32_t child = g::ReadU32(base, scene + kSceneFirstChild), n = 0; Pointer(child) && n < kMaxScenes;
       child = g::ReadU32(base, child + kSceneNextSibling), ++n) {
    TakeScene(base, child, depth + 1, out, census);
  }
  const uint32_t target = g::ReadU32(base, scene + kSceneColorTarget);
  if (Pointer(target) && !(g::ReadU32(base, target + kTargetFlags) & kTargetVirtualScreen)) {
    census.offscreen_scenes++;
    return;
  }
  census.scene_order[scene] = static_cast<int>(census.scene_order.size());
  const size_t solids_before = census.solids.size();
  TakeList(base, scene, g::ReadU32(base, scene + kSceneOpaque), false, out, census);
  TakeList(base, scene, g::ReadU32(base, scene + kSceneBlended), true, out, census);
  // The third list: the far generic surfaces, and the grass. 0x82145088 (which makes every generic surface's node)
  // puts a surface there instead of in the opaque list when the camera is farther than 0x825A231C times its object's
  // size (object +0x8C, its sphere's radius, scene +0x30), and the scene's render (0x8214F770) draws it right after
  // the opaque list. Its surfaces are the near ones' kind (side41: the same surfaces, formats and materials, soldiers
  // among them), so they are drawn (side42: the distance filled in, no crumpled shape as in side39); trees would be
  // left out, but none came. The first few far surfaces are shown in full. The grass needs its own drawing.
  const size_t solids = census.solids.size(), trees = census.trees.size();
  TakeList(base, scene, g::ReadU32(base, scene + kSceneLate), true, out, census);
  static int far_shown = 0;
  for (size_t i = solids; i < census.solids.size() && far_shown < kShownSurfaces; ++i) {
    REXLOG_INFO("cod3: ngl side: a far surface (third list), node {:08X}:", census.solids[i].node);
    ShowSurface(base, census.solids[i].node, far_shown);
  }
  census.far_solids += static_cast<int>(census.solids.size() - solids);
  census.far_trees += static_cast<int>(census.trees.size() - trees);
  census.trees.resize(trees);
  if (!census.world_scene && census.solids.size() > solids_before) census.world_scene = scene;
}

// side176: how each scene drawn on the screen clears its target, in the order they are drawn
void LogClears(uint8_t* base, uint64_t frame, const Census& census) {
  std::vector<std::pair<int, uint32_t>> scenes;
  for (const auto& [scene, order] : census.scene_order) scenes.emplace_back(order, scene);
  std::sort(scenes.begin(), scenes.end());
  for (const auto& [order, scene] : scenes) {
    if (order >= 12 || !Readable(scene + kSceneClearColor + 12)) continue;
    REXLOG_INFO("cod3: ngl side: frame {}: scene {} ({:08X}{}) clears with flags {:08X}, depth {:.3f}, color ({:.3f} "
                "{:.3f} {:.3f} {:.3f}; words {:08X} {:08X})",
                frame, order, scene, scene == census.world_scene ? ", the world's" : "",
                g::ReadU32(base, scene + kSceneClearFlags), g::ReadF32(base, scene + kSceneClearDepth),
                g::ReadF32(base, scene + kSceneClearColor), g::ReadF32(base, scene + kSceneClearColor + 4),
                g::ReadF32(base, scene + kSceneClearColor + 8), g::ReadF32(base, scene + kSceneClearColor + 12),
                g::ReadU32(base, scene + kSceneClearColor), g::ReadU32(base, scene + kSceneClearColor + 4));
  }
}

// The NGL texture as the listing shows it: its size and the Direct3D texture's format, tiling and packing.
std::string TextureText(uint8_t* base, uint32_t texture) {
  if (!Pointer(texture)) return "none";
  const uint32_t d3d = g::ReadU32(base, texture + kTextureDirect3D);
  char text[160];
  if (!Pointer(d3d)) {
    std::snprintf(text, sizeof text, "%08X (no Direct3D texture)", texture);
    return text;
  }
  const uint32_t f0 = g::ReadU32(base, d3d + kFetchConstant), f1 = g::ReadU32(base, d3d + kFetchConstant + 4);
  const uint32_t f2 = g::ReadU32(base, d3d + kFetchConstant + 8), f3 = g::ReadU32(base, d3d + kFetchConstant + 12);
  const uint32_t f5 = g::ReadU32(base, d3d + kFetchConstant + 20);
  std::snprintf(text, sizeof text, "%08X %ux%u format %u endian %u%s%s swizzle %03X flags %X", texture,
                (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, f1 & 0x3F, (f1 >> 6) & 3, (f0 >> 31) ? " tiled" : "",
                ((f5 >> 11) & 1) ? " packed" : "", (f3 >> 1) & 0xFFF, g::ReadU32(base, texture + kTextureFlags));
  return text;
}

// The first big dump's listing: each quad (where, its colors, its texture) and string (its text, font and place).
void List(uint8_t* base, uint64_t frame, const std::vector<Quad>& quads, const Census& census) {
  int listed = 0;
  for (const Quad& q : quads) {
    if (listed++ >= kListedQuads) break;
    const uint32_t target = Pointer(q.scene) ? g::ReadU32(base, q.scene + kSceneColorTarget) : 0;
    REXLOG_INFO("cod3: ngl side: frame {} quad {}: ({:.1f}, {:.1f}) to ({:.1f}, {:.1f}), uv ({:.3f}, {:.3f}) to "
                "({:.3f}, {:.3f}), colors {:08X} {:08X}, +50 {:08X} {:08X} {:08X} {:08X}, scene {:08X} target {:08X} "
                "flags {:X}, texture {}",
                frame, listed, q.x[0], q.y[0], q.x[3], q.y[3], q.u[0], q.v[0], q.u[3], q.v[3], q.color[0], q.color[3],
                q.extra[0], q.extra[1], q.extra[2], q.extra[3], q.scene, target,
                Pointer(target) ? g::ReadU32(base, target + kTargetFlags) : 0, TextureText(base, q.texture));
  }
  static std::unordered_set<uint32_t> fonts_shown;
  for (const Text& t : census.texts) {
    if (!Pointer(t.font) || !fonts_shown.insert(t.font).second) continue;
    for (uint32_t row = 0; row < 0x80; row += 0x20) {
      uint32_t w[8];
      for (uint32_t k = 0; k < 8; ++k) w[k] = g::ReadU32(base, t.font + row + 4 * k);
      REXLOG_INFO("cod3: ngl side: font {:08X} +{:02X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  t.font, row, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
  }
  int shown = 0;
  for (const Text& t : census.texts) {
    if (shown++ >= 16) break;
    std::string printable;
    for (const char c : t.text) printable += (c >= 32 && c < 127) ? c : '?';
    REXLOG_INFO("cod3: ngl side: frame {} string \"{}\": font {:08X} (cell height {}, first {}, {} glyphs, texture "
                "{}), at ({:.1f}, {:.1f}), scale {:.2f} x {:.2f}, color {:08X}, {} glyphs drawn",
                frame, printable.substr(0, 60), t.font,
                Pointer(t.font) ? g::ReadU32(base, t.font + kFontCellHeight) : 0,
                Pointer(t.font) ? g::ReadU32(base, t.font + kFontFirst) : 0,
                Pointer(t.font) ? g::ReadU32(base, t.font + kFontCount) : 0,
                Pointer(t.font) ? TextureText(base, g::ReadU32(base, t.font + kFontTexture)) : std::string("none"), t.x,
                t.y, t.scale_x, t.scale_y, t.color, t.glyphs);
  }
}

// The other nodes' vtables, most first: "82012345 x120, ...".
// side178: with each kind's Render (the vtable's third word), to name the kinds the native side doesn't draw
std::string OthersText(uint8_t* base, const Census& census) {
  std::vector<std::pair<int, uint32_t>> sorted;
  for (const auto& [vtable, n] : census.others) sorted.emplace_back(n, vtable);
  std::sort(sorted.rbegin(), sorted.rend());
  std::string text;
  char item[64];
  for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
    const uint32_t vtable = sorted[i].second;
    const uint32_t render = Pointer(vtable) && Readable(vtable) ? g::ReadU32(base, vtable + kVtableRender) : 0;
    std::snprintf(item, sizeof item, "%s%08X x%d (render %08X)", i ? ", " : "", vtable, sorted[i].first, render);
    text += item;
  }
  return text.empty() ? "none" : text;
}

// Helpers the native draws and the diagnostics share (also built without ngl_vk).
[[maybe_unused]] float VertexFloat(uint8_t* base, uint32_t at, uint32_t endian) {
  uint32_t word = GpuU32(base, at);  // as the GPU reads it with 8-in-32 swapping
  if (endian == 0) word = __builtin_bswap32(word);
  float f;
  std::memcpy(&f, &word, 4);
  return f;
}

// Diagnostics: count registers from first of a constant table (4 floats each), as one line.
std::string RegistersText(const float* c, int first, int count) {
  std::string text;
  char item[96];
  for (int r = first; r < first + count; ++r) {
    std::snprintf(item, sizeof item, " c%d=(%g %g %g %g)", r, c[4 * r], c[4 * r + 1], c[4 * r + 2], c[4 * r + 3]);
    text += item;
  }
  return text;
}

bool GrassReadable(uint32_t a) { return Readable(a) || (!(a & 3) && a >= 0x40000000u && a < 0x80000000u); }

// The grass's vertices where the game's own grass draw fetches them: vertex fetch constant 95's two words (the
// address, then the size and byte order), kept at each of the game's grass draws (OnDraw). The vertex buffer object
// in the manager (+0x04) is another buffer (224,000 bytes at 0x04EB3000; the drawn one is 84,000 at 0x04E9F000):
// side72 read the blades from it and they came out tens of units wide.
std::atomic<uint64_t> g_grass_fetch{0};

#if defined(COD3_NGL_VK)
std::filesystem::path ScreenshotFolder() {
  std::error_code error;
#if defined(_WIN32)
  // [rexglue-native] 2026-11-09: the program's path from Windows (no /proc/self/exe there; the working folder may be
  // anywhere)
  std::wstring name(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
  if (length == 0 || length >= name.size()) error = std::make_error_code(std::errc::no_such_file_or_directory);
  name.resize(error ? 0 : length);
  const std::filesystem::path exe(name);
#else
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
#endif
  std::filesystem::path folder = (error ? std::filesystem::current_path() : exe.parent_path().parent_path()) /
                                 "userdata" / "logs" / "screenshots";
  std::filesystem::create_directories(folder, error);
  return folder;
}

int g_state = 0;  // 0 not started, 1 ready, -1 failed
// side228: an NGL texture's ngl_vk texture and the fetch constant it was made from (a new texture at the same place
// has a different one: cod3_native_forget_level makes it again)
struct CachedTexture {
  uint32_t id = 0;  // 0: not drawable
  bool have_fetch = false;
  uint32_t fetch[6] = {};
};
std::unordered_map<uint32_t, CachedTexture> g_textures;  // NGL texture -> its ngl_vk texture
std::unordered_map<uint32_t, CachedTexture> g_cube_textures;  // rn34: NGL cube texture -> its ngl_vk cube (CubeFor)
uint64_t g_textures_made_again = 0;                       // side228: since the last log line
uint32_t g_forgets = 0;                                   // side228: ngl_vk_forget calls (ColorLookup's statics follow it)
// [rexglue-native] 2026-11-09: what the frame being drawn made new, for LiveFrame's slow-frame line and its totals: the
// textures (MakeTexture: reading the game's picture and its smaller levels, then ngl_vk_texture_levels) and the time
// they took, of it the time in ngl_vk_texture_levels, and the meshes (MeshFor). On LiveFrame's thread.
struct FrameMade {
  uint32_t textures = 0, meshes = 0;
  double texture_ms = 0.0, handed_ms = 0.0;
};
FrameMade g_frame_made;
std::unordered_set<uint32_t> g_cut_out;             // ngl_vk textures with see-through parts (leaves, grass cards)
std::unordered_set<std::string> g_reported;

// A texture has see-through parts when at least one texel in 64 has alpha under one half: a color map like that is
// drawn with the alpha test (bushes and grass cards; run side22 drew them as solid cards).
bool HasCutOut(const std::vector<uint8_t>& bgra) {
  size_t clear = 0;
  for (size_t at = 3; at < bgra.size(); at += 4) clear += bgra[at] < 128;
  return clear * 64 >= bgra.size() / 4;
}

// [acct2-bro] 2026-11-07 side160: the world lightmaps' alpha, once each (the first 48, with cod3_debug_ngl_side).
// side158-159 found the native picture too bright at night (falaise) and too dark by day (blkbrn); side165 found why:
// the alpha is how much of the sun reaches there (see cod3_debug_ngl_side_world_light). A world lightmap is a texture
// whose name starts with "lm" (lm0001 ...): its name, or "" when it isn't one.
std::string WorldLightmapName(uint8_t* base, uint32_t texture) {
  if (!IsTexture(base, texture)) return "";
  const uint32_t name = g::ReadU32(base, texture + kTextureName);
  if (!Pointer(name) || !Readable(name)) return "";
  const std::string text = InlineName(base, name + 4);
  return Lower(text).rfind("lm", 0) == 0 ? text : "";
}

void LogLightmapAlpha(uint8_t* base, uint32_t texture, const std::vector<uint8_t>& bgra, uint32_t width,
                      uint32_t height) {
  static int shown = 0;
  if (shown >= 48 || !REXCVAR_GET(cod3_debug_ngl_side)) return;
  const std::string text = WorldLightmapName(base, texture);
  if (text.empty()) return;
  const size_t texels = static_cast<size_t>(width) * height;
  if (texels == 0 || bgra.size() < texels * 4) return;
  ++shown;
  double color = 0, alpha = 0, color_by_alpha = 0;
  uint32_t lowest = 255, highest = 0;
  for (size_t i = 0; i < texels; ++i) {
    const uint8_t* t = &bgra[i * 4];
    const double c = (t[0] + t[1] + t[2]) / 3.0;
    color += c;
    alpha += t[3];
    color_by_alpha += c * t[3] / 255.0;
    lowest = std::min<uint32_t>(lowest, t[3]);
    highest = std::max<uint32_t>(highest, t[3]);
  }
  REXLOG_INFO("cod3: ngl side: lightmap {:08X} \"{}\" {}x{}: color mean {:.1f}, alpha mean {:.1f} (lowest {}, "
              "highest {}), color x alpha mean {:.1f}",
              texture, text, width, height, color / texels, alpha / texels, lowest, highest, color_by_alpha / texels);
}

// side165: a texture's fetch exponent (its fetch constant's dword 3, bits 13-18, signed: Xenia's exp_adjust): the GPU
// scales what it fetches by 2^this. The lightmapped world's light is scaled by it (WorldLight), in case a lightmap's
// texture has one; the first 16 asked for (lightmaps and color maps) are logged. (side163-164 read the materials'
// effect parameters, material +0x28: artists' values only (normal_scale, spec_power, detail_repeat ...), no light;
// NOTES 2026-11-07 11:25.)
int FetchExponent(uint8_t* base, uint32_t texture, const char* what) {
  if (!IsTexture(base, texture)) return 0;
  uint32_t fetch[6];
  if (!ReadFetch(base, GpuU32(base, texture + kTextureDirect3D), fetch)) return 0;
  const int raw = static_cast<int>((fetch[3] >> 13) & 0x3F);
  const int exponent = raw >= 32 ? raw - 64 : raw;
  static std::atomic<int> shown{0};
  if (shown.load(std::memory_order_relaxed) < 16 && shown.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_INFO("cod3: ngl side: {} {:08X}: fetch exponent {} (fetch constant {})", what, texture, exponent,
                Words(base, GpuU32(base, texture + kTextureDirect3D) + kFetchConstant, 6));
  }
  return exponent;
}

// side165: a lightmapped world draw's light (cod3_debug_ngl_side_world_light; ngl_vk.h lightmap_scale): the sun as its
// first light (its color is COD_SHADOW_PROJCOLOR, its way from COD_SHADOW_POSITION, as StateSun reads them), no more
// lights, the up part's share and the lightmap's fetch exponent as a factor. Without a sun known yet, nothing: the
// lightmap doubled, as before.
constexpr float kWorldLightUp = 0.2f;  // the shaders' literal: c255.z in 141294CDC7C97127, c254.x in E72CD0D5E1523920
void WorldLight(uint8_t* base, ngl_vk_mesh_draw& d, const Sun& sun, uint32_t lightmap, uint32_t color_map) {
  if (!sun.ok) return;
  LightWith(d, sun);
  d.extra_light_count = 0;
  d.light_saturate = 0;
  d.lightmap_scale = std::ldexp(1.0f, std::clamp(FetchExponent(base, lightmap, "world lightmap"), -8, 8));
  d.lightmap_up = kWorldLightUp;
  static std::atomic<int> colors_shown{0};  // the color maps' exponents, to see whether those scale too
  if (colors_shown.load(std::memory_order_relaxed) < 8 && colors_shown.fetch_add(1, std::memory_order_relaxed) < 8)
    FetchExponent(base, color_map, "world color map");
}

// side192: whether the texture being made is a sky layer's (SkyDraws), for LogLevelMeans
thread_local bool t_sky_texture = false;

// side192: each level's mean color against the top level's, for the sky's textures and for any texture whose smaller
// levels average far from its top level (decoded from the wrong place?); a few lines only
void LogLevelMeans(uint32_t label, const std::vector<uint8_t>& top, uint32_t width, uint32_t height,
                   const std::vector<std::vector<uint8_t>>& levels) {
  static std::atomic<int> sky_shown{0}, odd_shown{0};
  // [rexglue-native] 2026-11-09: only with cod3_debug_ngl_side (was: also in native mode, where every new texture's
  // levels were averaged for a few log lines)
  if (!REXCVAR_GET(cod3_debug_ngl_side) || top.empty()) return;
  auto mean = [](const std::vector<uint8_t>& bgra) {
    uint64_t sum = 0;
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) sum += bgra[i] + bgra[i + 1] + bgra[i + 2];
    const size_t n = bgra.size() / 4;
    return n ? static_cast<double>(sum) / (3.0 * static_cast<double>(n)) : 0.0;
  };
  const double top_mean = mean(top);
  std::string text;
  bool odd = false;
  for (const std::vector<uint8_t>& level : levels) {
    const double m = mean(level);
    if (top_mean > 16.0 && std::fabs(m - top_mean) > 0.15 * top_mean) odd = true;
    char item[16];
    std::snprintf(item, sizeof item, " %.0f", m);
    text += item;
  }
  if (t_sky_texture ? sky_shown.fetch_add(1, std::memory_order_relaxed) >= 16
                    : (!odd || odd_shown.fetch_add(1, std::memory_order_relaxed) >= 40)) {
    return;
  }
  REXLOG_INFO("cod3: ngl side: texture {:08X} {}x{}{}: mean brightness, top level {:.0f}, smaller levels:{}", label,
              width, height, t_sky_texture ? " (sky)" : "", top_mean, text.empty() ? std::string(" none") : text);
}

// An ngl_vk texture for the picture a fetch constant names: the game's own (or a pack's in its place) with its
// smaller levels. label names it in the log (the NGL texture, or the texture's address for a draw's fetch constant).
// blended: the texture is only ever drawn blended (the effects' quads, the sky's layers), never with the alpha test.
uint32_t MakeTexture(uint8_t* base, const uint32_t* fetch, uint32_t label, std::string& why, bool blended = false) {
  struct Timed {  // [rexglue-native] 2026-11-09: counted in g_frame_made, however it returns
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Timed() {
      ++g_frame_made.textures;
      g_frame_made.texture_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
  } timed;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  if (!DecodeFetch(base, fetch, bgra, width, height, why)) return 0;
  LogLightmapAlpha(base, label, bgra, width, height);  // side160
  // [rexglue-native] 2026-11-09 rn10: a texture drawn blended keeps the game's own smaller levels as they are.
  // KeepCoverage (for the alpha test) raised a smoke flipbook's far levels' alpha until each frame of it was a solid
  // square: a far dust cloud became a heap of white-edged squares (owner 14:03: "smoke effects are boxes").
  const bool cut_out = !blended && HasCutOut(bgra);  // from the game's own picture
  {  // side173: the small textures' mean color as decoded (Black Burn's 16 x 16 sky base comes out black natively)
    static int small_shown = 0;
    if (Watching() && width * height <= 256 && small_shown < 24 && !bgra.empty()) {
      ++small_shown;
      uint64_t sum[4] = {};
      for (size_t i = 0; i + 3 < bgra.size(); i += 4)
        for (int k = 0; k < 4; ++k) sum[k] += bgra[i + k];
      const uint64_t n = bgra.size() / 4;
      REXLOG_INFO("cod3: ngl side: small texture {:08X} {}x{} format {} endian {} packed {} tiled {}: mean blue {} "
                  "green {} red {} alpha {}{}",
                  label, width, height, fetch[1] & 0x3F, (fetch[1] >> 6) & 3, (fetch[5] >> 11) & 1, fetch[0] >> 31,
                  sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, cut_out ? ", cut out" : "");
    }
  }
  static int keys_shown = 0;  // every texture's pack key, to check against the ripper's (hd_keys.tsv, and the
                              // insignia keys in docs/TEXTURE_REPLACEMENT.md)
  uint64_t key;
  uint32_t format;
  // [rexglue-native] 2026-11-09: only with cod3_debug_ngl_side (was: in native mode too, a second read and hash of
  // every new texture for a log line)
  if (REXCVAR_GET(cod3_debug_ngl_side) && keys_shown < 4000 && TextureKeyFetch(base, fetch, key, format)) {
    ++keys_shown;
    REXLOG_INFO("cod3: ngl side: texture {:08X} key {:016x} ({}x{} format {})", label, key, width, height, format);
  }
  std::vector<uint8_t> hd;
  uint32_t hd_width = 0, hd_height = 0;
  // the smaller levels: the game's own, or made from a pack's picture (owner: far textures shimmered with the top
  // level alone); a cut-out's keep its coverage
  std::vector<std::vector<uint8_t>> levels;
  if (PackPicture(base, fetch, bgra, width, height, hd, hd_width, hd_height)) {
    bgra.swap(hd);
    width = hd_width;
    height = hd_height;
    BoxLevels(bgra, width, height, levels);
  } else {
    DecodeLevelsFetch(base, fetch, levels);
    LogLevelMeans(label, bgra, width, height, levels);  // side192
    const int32_t level_mode = REXCVAR_GET(cod3_debug_ngl_side_texture_levels);
    if (level_mode == 1) {
      BoxLevels(bgra, width, height, levels);
    } else if (level_mode == 2) {
      levels.clear();
    }
  }
  if (cut_out) KeepCoverage(bgra, levels);
  std::vector<const void*> pictures{bgra.data()};
  for (const std::vector<uint8_t>& level : levels) pictures.push_back(level.data());
  const auto handed_at = std::chrono::steady_clock::now();  // [rexglue-native] 2026-11-09
  const uint32_t id =
      ngl_vk_texture_levels(pictures.data(), static_cast<uint32_t>(pictures.size()), width, height);
  g_frame_made.handed_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - handed_at).count();
  static int levels_shown = 0;  // diagnostics: the first textures' level counts
  if (levels_shown < 12) {
    ++levels_shown;
    REXLOG_INFO("cod3: ngl side: texture {:08X} {}x{}: {} levels", label, width, height, pictures.size());
  }
  if (!id) why = "ngl_vk_texture failed";
  if (id && cut_out) g_cut_out.insert(id);
  return id;
}

uint32_t TextureFor(uint8_t* base, uint32_t texture) {
  if (!Pointer(texture)) return 0;
  if (g::ReadU32(base, texture + kTextureFlags) & kTextureAnimated) {
    const uint32_t frames = g::ReadU32(base, texture + kTextureFrames);
    if (Pointer(frames) && Pointer(g::ReadU32(base, frames))) texture = g::ReadU32(base, frames);
  }
  uint32_t fetch[6];
  bool fetched = false, have_fetch = false;
  const auto found = g_textures.find(texture);
  if (found != g_textures.end()) {
    if (!REXCVAR_GET(cod3_native_forget_level)) return found->second.id;
    // side228: the same place, the same fetch constant: the same picture
    have_fetch = ReadFetch(base, g::ReadU32(base, texture + kTextureDirect3D), fetch);
    fetched = true;
    const CachedTexture& c = found->second;
    if (c.have_fetch == have_fetch && (!have_fetch || std::memcmp(c.fetch, fetch, sizeof fetch) == 0)) return c.id;
    ++g_textures_made_again;  // the old ngl_vk texture stays until the next ngl_vk_forget
  }
  if (!fetched) have_fetch = ReadFetch(base, g::ReadU32(base, texture + kTextureDirect3D), fetch);
  std::string why = "no Direct3D texture";
  const uint32_t id = have_fetch ? MakeTexture(base, fetch, texture, why) : 0;
  if (!id && g_reported.insert(why).second) {
    REXLOG_INFO("cod3: ngl side: texture {:08X} not drawn: {} (others like it aren't named again)", texture, why);
  }
  CachedTexture& c = g_textures[texture];
  c.id = id;
  c.have_fetch = have_fetch;
  if (have_fetch) std::memcpy(c.fetch, fetch, sizeof fetch);
  return id;
}

// [rexglue-native] 2026-11-09 rn34: cube textures (the guns' environment, CFC45995D1294CCE's tf2). A fetch constant's
// dword 5 bits 9-10 are its dimension (xtex.py's header: 1 2D; 3 a cube).
constexpr uint32_t kDimensionCube = 3;
bool CubeFetch(const uint32_t* fetch) { return ((fetch[5] >> 9) & 3) == kDimensionCube; }

bool IsCubeTexture(uint8_t* base, uint32_t texture) {
  uint32_t fetch[6];
  return Pointer(texture) && IsTexture(base, texture) &&
         ReadFetch(base, g::ReadU32(base, texture + kTextureDirect3D), fetch) && CubeFetch(fetch);
}

// A cube's six faces (Direct3D's order: +x, -x, +y, -y, +z, -z), each its top level as BGRA8 rows. The faces are the
// texture's six slices, one after another, each laid out as a 2D texture's top level (DecodeFetch: tiled, pitch and
// rows rounded up to 32 blocks) and starting on a 4 KB boundary (the GPU's subresource alignment, as Xenia lays slices
// out). Not for a cube small enough that its top level sits in a packed tail (16 texels or less).
bool DecodeCubeFaces(uint8_t* base, const uint32_t* fetch, std::vector<uint8_t> (&faces)[6], uint32_t& size,
                     std::string& why) {
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  const bool packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if (width != height) {
    why = "a cube whose faces aren't square";
    return false;
  }
  if (packed && width <= 16) {
    why = "a cube small enough to be in a packed tail";
    return false;
  }
  if (!FormatOf(format, &f)) {
    why = "cube format " + std::to_string(format);
    return false;
  }
  const uint32_t blocks = (width + f.block - 1) / f.block;
  uint32_t pitch_blocks = std::max(pitch / f.block, blocks);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const uint32_t rows = tiled ? (blocks + 31) & ~31u : blocks;
  const size_t face_bytes = static_cast<size_t>(pitch_blocks) * rows * f.bytes;
  const size_t stride = (face_bytes + 4095) & ~size_t{4095};
  if (stride * 6 > kMaxTextureBytes || physical == 0) {
    why = "cube size or address out of range";
    return false;
  }
  for (uint32_t face = 0; face < 6; ++face) {
    const uint32_t at = physical + static_cast<uint32_t>(stride * face);
    DecodeSurface(GpuPointer(base, 0xA0000000u | at), std::min(face_bytes, PhysicalRoom(at)), f, format, endian,
                  swizzle, tiled, pitch_blocks, 0, 0, width, height, faces[face]);
  }
  size = width;
  return true;
}

// An NGL cube texture's ngl_vk cube (ngl_vk_texture_cube): its six faces and their smaller levels averaged from them
// (BoxLevels; the game's own smaller levels of a cube aren't read yet). With cod3_debug_ngl_side, the first two cubes
// are also saved as a strip of their faces (userdata/logs/screenshots/ngl-side-cube-<address>.ppm), to check the faces'
// order and layout by eye.
uint32_t CubeFor(uint8_t* base, uint32_t texture) {
  if (!Pointer(texture)) return 0;
  uint32_t fetch[6];
  const bool have_fetch = ReadFetch(base, g::ReadU32(base, texture + kTextureDirect3D), fetch);
  const auto found = g_cube_textures.find(texture);
  if (found != g_cube_textures.end()) {
    const CachedTexture& c = found->second;
    if (c.have_fetch == have_fetch && (!have_fetch || std::memcmp(c.fetch, fetch, sizeof fetch) == 0)) return c.id;
  }
  std::string why = "no Direct3D texture";
  std::vector<uint8_t> faces[6];
  uint32_t size = 0, id = 0;
  if (have_fetch && !CubeFetch(fetch)) why = "not a cube";
  if (have_fetch && CubeFetch(fetch) && DecodeCubeFaces(base, fetch, faces, size, why)) {
    std::vector<std::vector<uint8_t>> levels[6];
    for (uint32_t face = 0; face < 6; ++face) BoxLevels(faces[face], size, size, levels[face]);
    const size_t level_count = 1 + levels[0].size();
    std::vector<const void*> pictures;
    for (size_t level = 0; level < level_count; ++level) {
      for (uint32_t face = 0; face < 6; ++face) {
        pictures.push_back(level == 0 ? faces[face].data()
                           : level - 1 < levels[face].size() ? levels[face][level - 1].data() : nullptr);
      }
    }
    id = ngl_vk_texture_cube(pictures.data(), static_cast<uint32_t>(level_count), size);
    if (!id) why = "ngl_vk_texture_cube failed";
    static int saved = 0;
    if (REXCVAR_GET(cod3_debug_ngl_side) && saved < 2) {
      ++saved;
      std::vector<uint8_t> strip(static_cast<size_t>(size) * 6 * size * 4);
      for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < size; ++y) {
          for (uint32_t x = 0; x < size; ++x) {
            const uint8_t* from = &faces[face][(static_cast<size_t>(y) * size + x) * 4];
            uint8_t* to = &strip[(static_cast<size_t>(y) * size * 6 + face * size + x) * 4];
            to[0] = from[2];  // BGRA to RGBA
            to[1] = from[1];
            to[2] = from[0];
            to[3] = from[3];
          }
        }
      }
      char name[64];
      std::snprintf(name, sizeof name, "ngl-side-cube-%08X.ppm", texture);
      SavePpm(ScreenshotFolder() / name, strip.data(), size * 6, size);
      REXLOG_INFO("cod3: ngl side: rn34 cube {:08X}: {}x{} faces, format {}, tiled {}, fetch {}, saved as {}", texture,
                  size, size, fetch[1] & 0x3F, fetch[0] >> 31, Words(base, g::ReadU32(base, texture + kTextureDirect3D)
                  + kFetchConstant, 6), name);
    }
  }
  if (!id && g_reported.insert(why).second) {
    REXLOG_INFO("cod3: ngl side: cube {:08X} not drawn: {} (others like it aren't named again)", texture, why);
  }
  CachedTexture& c = g_cube_textures[texture];
  c.id = id;
  c.have_fetch = have_fetch;
  if (have_fetch) std::memcpy(c.fetch, fetch, sizeof fetch);
  return id;
}

void AddQuad(std::vector<ngl_vk_vertex>& v, const Quad& q, uint32_t texture) {
  static const int kCorners[6] = {0, 1, 2, 2, 1, 3};
  for (const int c : kCorners) {
    ngl_vk_vertex out;
    out.x = q.x[c];
    out.y = q.y[c] * kScreenSqueeze;
    out.u = q.u[c];
    out.v = q.v[c];
    out.color = q.color[c];
    out.textured = texture ? 2.0f : 0.0f;
    out.texture = texture;
    v.push_back(out);
  }
}

void SavePpm(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (!file) return;
  std::fprintf(file, "P6\n%u %u\n255\n", width, height);
  std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) std::memcpy(&row[x * 3], rgba + (static_cast<size_t>(y) * width + x) * 4, 3);
    std::fwrite(row.data(), 1, row.size(), file);
  }
  std::fclose(file);
}

// ---------------------------------------------------------------------------------------------------------------
// Step 4's 3D (see the top).

// A scene's camera for ngl_vk: its view-projection, depth turned over when it runs backward. False when it isn't a
// perspective camera (a shadow's, say) or its numbers aren't sane.
constexpr float kFrontDepth = 0.02f;  // the share of the depth range a scene drawn in front of the world gets
constexpr float kFrontNear = 0.1f;    // ... a scene whose near plane is closer than this share of the world's

// The distance to a scene's near plane, from its projection (row vectors: depth = (a z + b) / (c z + d), d being 0
// for a perspective): where depth is 1 when it runs backward, 0 when forward; the smaller positive one is the near
// plane either way (the other is the far). 0 when it can't be told.
float NearPlane(uint8_t* base, uint32_t scene) {
  const float a = g::ReadF32(base, scene + kSceneProjection + 4 * 10);
  const float b = g::ReadF32(base, scene + kSceneProjection + 4 * 14);
  const float c = g::ReadF32(base, scene + kSceneProjection + 4 * 11);
  float best = 0.0f;
  for (const float z : {c != a ? b / (c - a) : 0.0f, a != 0.0f ? -b / a : 0.0f}) {
    if (std::isfinite(z) && z > 0.0f && (best == 0.0f || z < best)) best = z;
  }
  return best;
}

bool SceneCamera(uint8_t* base, uint32_t scene, float view_projection[16], bool* reversed) {
  float projection[16];
  float sum = 0.0f;
  for (uint32_t k = 0; k < 16; ++k) {
    view_projection[k] = g::ReadF32(base, scene + kSceneViewProjection + 4 * k);
    projection[k] = g::ReadF32(base, scene + kSceneProjection + 4 * k);
    if (!std::isfinite(view_projection[k]) || !std::isfinite(projection[k])) return false;
    sum += std::fabs(view_projection[k]);
  }
  if (sum == 0.0f) return false;
  // the w column (Direct3D row-vector: element [row][column] is at 4 * row + column) moves with the position
  if (std::fabs(view_projection[3]) + std::fabs(view_projection[7]) + std::fabs(view_projection[11]) < 1e-6f) {
    return false;
  }
  auto depth = [&projection](float z) {
    const float w = z * projection[11] + projection[15];
    return (z * projection[10] + projection[14]) / (w != 0.0f ? w : 1.0f);
  };
  *reversed = depth(1000.0f) < depth(10.0f);
  if (*reversed) {
    for (int row = 0; row < 4; ++row) {
      view_projection[4 * row + 2] = view_projection[4 * row + 3] - view_projection[4 * row + 2];
    }
  }
  return true;
}

// [rexglue-native] 2026-11-09: split screen (cod3mp's 2 to 4 local players). Each player's scene has its own
// projection, and it already puts that player's view in its part of the screen: its row 2's x and y (the scene's
// +0x150 and +0x154; Direct3D's row vectors, w from row 2's last) are that part's center in clip space, 0.5 from the
// middle across a split, 0 across a whole side. rn2's 4-player mp_merv: (-0.5, 0.5) (0.5, 0.5) (-0.5, -0.5) (0.5, -0.5),
// the projection's x and y scales halved. The game keeps each view inside its part; ngl_vk needs a scissor for that
// (else what a view sees past its edge lands in the next player's part). Sets the draw's scissor to the scene's part
// of the native picture and returns true for such a scene; a whole-screen scene (one player, every campaign scene)
// gets none and false.
// SplitPart: the same part as a fraction of the picture (left, top, right, bottom, from its top-left corner, 0 to 1),
// also for the 2D quads (rn6: each player's clearing quad comes just before that player's 3D).
bool SplitPart(uint8_t* base, uint32_t scene, float part[4]) {
  const float w = g::ReadF32(base, scene + kSceneProjection + 4 * 11);
  float cx = g::ReadF32(base, scene + kSceneProjection + 4 * 8);
  float cy = g::ReadF32(base, scene + kSceneProjection + 4 * 9);
  if (!std::isfinite(w) || !std::isfinite(cx) || !std::isfinite(cy) || std::fabs(w) < 1e-6f) return false;
  cx /= w;
  cy /= w;
  auto side = [](float c, bool& split) {  // a split side: 0.5 from the middle; a whole side: 0
    split = std::fabs(std::fabs(c) - 0.5f) < 0.02f;
    return split || std::fabs(c) < 0.02f;
  };
  bool split_x = false, split_y = false;
  if (!side(cx, split_x) || !side(cy, split_y) || !(split_x || split_y)) return false;
  const float half_x = split_x ? 0.5f : 1.0f, half_y = split_y ? 0.5f : 1.0f;
  // clip space (x right, y up, -1 to 1) to the picture's (from the top-left corner, 0 to 1)
  part[0] = (cx - half_x + 1.0f) * 0.5f;
  part[1] = (1.0f - (cy + half_y)) * 0.5f;
  part[2] = (cx + half_x + 1.0f) * 0.5f;
  part[3] = (1.0f - (cy - half_y)) * 0.5f;
  return true;
}

bool SplitScissor(uint8_t* base, uint32_t scene, ngl_vk_mesh_draw& d) {
  float part[4];
  if (!SplitPart(base, scene, part)) return false;
  const float left = part[0], top = part[1], right = part[2], bottom = part[3];
  const auto x0 = static_cast<int32_t>(std::lround(left * static_cast<float>(g_target_width)));
  const auto x1 = static_cast<int32_t>(std::lround(right * static_cast<float>(g_target_width)));
  const auto y0 = static_cast<int32_t>(std::lround(top * static_cast<float>(g_target_height)));
  const auto y1 = static_cast<int32_t>(std::lround(bottom * static_cast<float>(g_target_height)));
  if (x1 <= x0 || y1 <= y0) return false;
  d.scissor_x = x0;
  d.scissor_y = y0;
  d.scissor_width = static_cast<uint32_t>(x1 - x0);
  d.scissor_height = static_cast<uint32_t>(y1 - y0);
  return true;
}

// side223: the world scene's camera as SceneCamera gives it, this frame's (set by SolidDraws) and the one before
// (EffectDraws), on the thread that draws the native frames
struct WorldCamera {
  bool ok = false;
  float view_projection[16] = {};
};
WorldCamera g_world_camera, g_world_camera_before;

// Whether two of SceneCamera's view-projections are the same camera (within a part in a thousand of the largest)
bool SameCamera(const float* a, const float* b) {
  float largest = 0.0f, apart = 0.0f;
  for (int k = 0; k < 16; ++k) {
    largest = std::max(largest, std::max(std::fabs(a[k]), std::fabs(b[k])));
    apart = std::max(apart, std::fabs(a[k] - b[k]));
  }
  return largest > 0.0f && apart <= 1e-3f * largest;
}

// side125: the world scene's depth (ngl_vk's: 0 near to 1 far, turned over as SceneCamera turns it) at a distance
// from the eye, from its projection, for the depth of field's distances (DepthOfField).
struct WorldDepth {
  bool ok = false, reversed = false;
  float p10 = 0.0f, p11 = 0.0f, p14 = 0.0f, p15 = 0.0f;
};
WorldDepth g_world_depth;  // this frame's (the main thread's)

WorldDepth WorldDepthOf(uint8_t* base, uint32_t scene) {
  WorldDepth w;
  if (!scene) return w;
  w.p10 = g::ReadF32(base, scene + kSceneProjection + 4 * 10);
  w.p11 = g::ReadF32(base, scene + kSceneProjection + 4 * 11);
  w.p14 = g::ReadF32(base, scene + kSceneProjection + 4 * 14);
  w.p15 = g::ReadF32(base, scene + kSceneProjection + 4 * 15);
  w.ok = std::isfinite(w.p10) && std::isfinite(w.p11) && std::isfinite(w.p14) && std::isfinite(w.p15) &&
         std::fabs(w.p11) + std::fabs(w.p15) > 0.0f;
  return w;
}

float DepthAt(const WorldDepth& w, float distance) {
  const float wd = distance * w.p11 + w.p15;
  const float d = (distance * w.p10 + w.p14) / (wd != 0.0f ? wd : 1.0f);
  return d;
}

struct Geometry {
  uint32_t vertices = 0, vertex_start = 0, vertex_count = 0, vertex_bytes = 0, stride = 0, endian = 0;
  uint32_t indices = 0, index_start = 0, index_count = 0, index_bytes = 0, index_size = 2;
  int32_t position = -1;            // byte offsets in a vertex (-1: none)
  int32_t uv[3] = {-1, -1, -1};     // texture coordinates 0, 1 and 2 (side195: the terrain's second layer's)
  bool uv_half[3] = {false, false, false};  // as two 16-bit floats, else two floats
  int32_t color = -1;               // side195: the corners' color (D3DCOLOR; -1: none)
  int32_t blend_weights = -1, blend_indices = -1;  // a skinned vertex's (offsets; -1: none)
  uint32_t blend_weights_type = 0, blend_indices_type = 0;
  int32_t normal = -1;              // side134: the normal's offset (-1: none) and its type
  uint32_t normal_type = 0;
};

// Xbox 360 vertex element types (D3DDECLTYPE) and uses (D3DDECLUSAGE) read here.
constexpr uint32_t kDeclFloat2 = 0x2C23A5, kDeclFloat3 = 0x2A23B9, kDeclHalf2 = 0x2C235F;
constexpr uint32_t kUsagePosition = 0, kUsageBlendWeight = 1, kUsageBlendIndices = 2, kUsageNormal = 3,
                   kUsageTexcoord = 5, kUsageColor = 10;
// A type's low six bits are the GPU's vertex format; 0x100 marks a signed one, 0x200 one read as whole numbers.
constexpr uint32_t kFormat2_10_10_10 = 0x07, kFormat32_32_32Float = 0x39, kTypeSigned = 0x100, kTypeWhole = 0x200;

// The surface's vertex declaration (surface +0x60: +0x00 the stride, +0x04 the elements, 12 bytes each: stream and
// offset (16 bits each), type, then method, use, use index, a pad byte; ended by stream 0xFF).
void ReadFormat(uint8_t* base, uint32_t surface, Geometry& geo) {
  const uint32_t format = g::ReadU32(base, surface + kSurfaceFormat);
  if (!Pointer(format)) return;
  geo.stride = g::ReadU32(base, format);
  const uint32_t elements = g::ReadU32(base, format + 4);
  if (!Pointer(elements)) return;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t e = elements + 12 * i;
    const uint32_t where = g::ReadU32(base, e), type = g::ReadU32(base, e + 4), use = g::ReadU32(base, e + 8);
    const uint32_t stream = where >> 16, offset = where & 0xFFFF;
    if (stream == 0xFF || stream == 0xFFFF) break;
    if (stream != 0) continue;
    const uint32_t usage = (use >> 16) & 0xFF, index = (use >> 8) & 0xFF;
    if (usage == kUsagePosition && index == 0 && type == kDeclFloat3) geo.position = static_cast<int32_t>(offset);
    if (usage == kUsageBlendWeight && index == 0) {
      geo.blend_weights = static_cast<int32_t>(offset);
      geo.blend_weights_type = type;
    }
    if (usage == kUsageBlendIndices && index == 0) {
      geo.blend_indices = static_cast<int32_t>(offset);
      geo.blend_indices_type = type;
    }
    if (usage == kUsageNormal && index == 0) {
      geo.normal = static_cast<int32_t>(offset);
      geo.normal_type = type;
    }
    if (usage == kUsageColor && index == 0) geo.color = static_cast<int32_t>(offset);  // side195
    if (usage == kUsageTexcoord && index < 3 && (type == kDeclFloat2 || type == kDeclHalf2)) {
      geo.uv[index] = static_cast<int32_t>(offset);
      geo.uv_half[index] = type == kDeclHalf2;
    }
  }
}

bool ReadGeometry(uint8_t* base, uint32_t surface, Geometry& geo, std::string& why) {
  for (uint32_t k = 0; k < 2; ++k) {
    const uint32_t at = surface + kSurfaceBuffers + 0x10 * k;
    const uint32_t buffer = g::ReadU32(base, at);
    if (!Pointer(buffer)) continue;
    const uint32_t f0 = g::ReadU32(base, buffer + kBufferFetch), f1 = g::ReadU32(base, buffer + kBufferFetch + 4);
    if ((f0 & 3) == 3) {  // a vertex fetch constant: address, then endianness and size in words
      geo.vertices = GuestAddress(f0 & ~3u);
      geo.endian = f1 & 3;
      geo.vertex_bytes = ((f1 >> 2) & 0xFFFFFF) * 4;
      geo.vertex_start = g::ReadU32(base, at + 4);
      geo.vertex_count = g::ReadU32(base, at + 8);
    } else {
      geo.indices = GuestAddress(f0);
      geo.index_bytes = f1;
      geo.index_start = g::ReadU32(base, at + 4);
      geo.index_count = g::ReadU32(base, at + 8);
      geo.index_size = g::ReadU32(base, at + 12) == 4 ? 4 : 2;
    }
  }
  geo.stride = g::ReadU32(base, surface + kSurfaceStride);
  ReadFormat(base, surface, geo);
  if (!geo.vertices || !geo.indices) {
    why = "no vertex or index buffer";
  } else if (!Physical(geo.vertices) || !Physical(geo.indices)) {
    why = "a buffer outside physical memory";
  } else if (geo.stride < 12 || geo.stride > 256 || (geo.stride & 3)) {
    why = "stride not 12..256";
  } else if (geo.position < 0 || static_cast<uint32_t>(geo.position) + 12 > geo.stride) {
    why = "no three-float position";
  } else if (geo.endian != 0 && geo.endian != 2) {
    why = "vertex endianness " + std::to_string(geo.endian);
  } else if (geo.vertex_count == 0 || geo.vertex_count > 65536 || geo.index_count < 3 || geo.index_count > (1u << 22)) {
    why = "counts out of range";
  } else if ((static_cast<uint64_t>(geo.vertex_start) + geo.vertex_count) * geo.stride > geo.vertex_bytes) {
    why = "vertices past their buffer";
  } else if ((static_cast<uint64_t>(geo.index_start) + geo.index_count) * geo.index_size > geo.index_bytes) {
    why = "indices past their buffer";
  } else {
    return true;
  }
  return false;
}

float HalfFloat(uint32_t h) {
  const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
  float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                              : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
  if (exponent == 31) value = 0.0f;  // infinities and NaNs: none in texture coordinates
  return sign ? -value : value;
}

// Texture coordinates k of the vertex at `at` into u, v (0, 0 when the format has none).
void VertexUv(uint8_t* base, uint32_t at, const Geometry& geo, int k, float* u, float* v) {
  *u = *v = 0.0f;
  if (geo.uv[k] < 0 || static_cast<uint32_t>(geo.uv[k]) + (geo.uv_half[k] ? 4u : 8u) > geo.stride) return;
  const uint32_t where = at + static_cast<uint32_t>(geo.uv[k]);
  if (geo.uv_half[k]) {
    uint32_t word = GpuU32(base, where);
    if (geo.endian == 0) word = __builtin_bswap32(word);
    *u = HalfFloat(word >> 16);  // the first of the two in the word's high half (8-in-32 order)
    *v = HalfFloat(word & 0xFFFF);
  } else {
    *u = VertexFloat(base, where, geo.endian);
    *v = VertexFloat(base, where + 4, geo.endian);
  }
}

// side134: the vertex at `at`'s normal into out (false, and 0, 0, 0, when the format has none or one not read here).
// The arms' vertex shader (shader_C425E144C8047506) fetches theirs as FMT_2_10_10_10, signed: x in the low ten bits.
bool VertexNormal(uint8_t* base, uint32_t at, const Geometry& geo, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  if (geo.normal < 0) return false;
  const uint32_t where = at + static_cast<uint32_t>(geo.normal);
  const uint32_t format = geo.normal_type & 0x3F;
  if (format == kFormat32_32_32Float && static_cast<uint32_t>(geo.normal) + 12 <= geo.stride) {
    for (int k = 0; k < 3; ++k) out[k] = VertexFloat(base, where + 4 * k, geo.endian);
  } else if (format == kFormat2_10_10_10 && static_cast<uint32_t>(geo.normal) + 4 <= geo.stride) {
    uint32_t word = GpuU32(base, where);
    if (geo.endian == 0) word = __builtin_bswap32(word);
    const bool whole = (geo.normal_type & kTypeWhole) != 0;
    for (int k = 0; k < 3; ++k) {
      const uint32_t bits = (word >> (10 * k)) & 0x3FF;
      if (geo.normal_type & kTypeSigned) {
        const float value = static_cast<float>(static_cast<int32_t>(bits << 22) >> 22);
        out[k] = whole ? value : std::max(value / 511.0f, -1.0f);
      } else {
        out[k] = whole ? static_cast<float>(bits) : static_cast<float>(bits) / 1023.0f;
      }
    }
  } else {
    static std::mutex mutex;
    static std::vector<uint32_t> said;
    std::lock_guard<std::mutex> lock(mutex);
    if (said.size() < 8 && std::find(said.begin(), said.end(), geo.normal_type) == said.end()) {
      said.push_back(geo.normal_type);
      REXLOG_INFO("cod3: ngl side: a normal of type {:06X} (stride {}, at +{}) isn't read; the triangle's is used",
                  geo.normal_type, geo.stride, geo.normal);
    }
    return false;
  }
  const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  if (!std::isfinite(length) || length < 0.1f || length > 2.0f) {
    out[0] = out[1] = out[2] = 0.0f;
    return false;
  }
  return true;
}

// side228: a surface's ngl_vk mesh and the counts it was made with (a new surface at the same place has others)
struct CachedMesh {
  uint32_t id = 0;  // 0: not drawable
  uint32_t vertex_count = 0, index_count = 0, stride = 0;
};
std::unordered_map<uint64_t, CachedMesh> g_meshes;  // (vertices, indices) -> ngl_vk mesh
std::unordered_map<uint64_t, CachedMesh> g_second_meshes;  // side195: the same for the terrain's second layer
uint64_t g_meshes_made_again = 0;  // side228: since the last log line
bool g_meshes_full = false;

// side195: a corner's weight for the terrain's second layer, the alpha of its color (FMT_8_8_8_8's w, the word's top
// byte as the GPU reads it), as a corner color (white, that alpha).
uint32_t SecondLayerWeight(uint8_t* base, uint32_t at, const Geometry& geo) {
  if (geo.color < 0 || static_cast<uint32_t>(geo.color) + 4 > geo.stride) return 0x00FFFFFFu;
  uint32_t word = GpuU32(base, at + static_cast<uint32_t>(geo.color));
  if (geo.endian == 0) word = __builtin_bswap32(word);
  return (word & 0xFF000000u) | 0x00FFFFFFu;
}

// The surface's mesh, made once; 0 (and why) when it can't be. side195: second_layer, the same corners with the
// terrain's second layer's coordinates in the base texture's place and its weight in the color's alpha.
uint32_t MeshFor(uint8_t* base, uint32_t surface, std::string& why, bool second_layer = false) {
  Geometry geo;
  if (!ReadGeometry(base, surface, geo, why)) return 0;
  if (second_layer && (geo.uv[2] < 0 || geo.color < 0)) {
    why = "no third texture coordinates or color for its second layer";
    return 0;
  }
  const uint64_t key = static_cast<uint64_t>(geo.vertices + geo.vertex_start * geo.stride) << 32 |
                       (geo.indices + geo.index_start * geo.index_size);
  auto& meshes = second_layer ? g_second_meshes : g_meshes;
  const auto found = meshes.find(key);
  if (found != meshes.end()) {
    const CachedMesh& c = found->second;
    // side228: the same counts, the same surface (without cod3_native_forget_level, always)
    if (!REXCVAR_GET(cod3_native_forget_level) ||
        (c.vertex_count == geo.vertex_count && c.index_count == geo.index_count && c.stride == geo.stride)) {
      if (!c.id) why = "left out before";
      return c.id;
    }
    ++g_meshes_made_again;  // the old ngl_vk mesh stays until the next ngl_vk_forget
  }
  CachedMesh& cached = meshes[key];
  cached = CachedMesh{0, geo.vertex_count, geo.index_count, geo.stride};
  uint32_t& mesh = cached.id;
  float low[3], high[3];
  for (int k = 0; k < 3; ++k) {
    low[k] = g::ReadF32(base, surface + kSurfaceBoxLow + 4 * k);
    high[k] = g::ReadF32(base, surface + kSurfaceBoxHigh + 4 * k);
  }
  const float pad = 1.0f + 0.05f * std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
  std::vector<float> corners(static_cast<size_t>(geo.vertex_count) * NGL_VK_CORNER_FLOATS, 0.0f);
  const uint32_t white = 0xFFFFFFFFu;
  const bool normals = REXCVAR_GET(cod3_debug_ngl_side_normals);
  for (uint32_t i = 0; i < geo.vertex_count; ++i) {
    const uint32_t at = geo.vertices + (geo.vertex_start + i) * geo.stride;
    float* c = &corners[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    VertexUv(base, at, geo, second_layer ? 2 : 0, &c[3], &c[4]);
    VertexUv(base, at, geo, 1, &c[5], &c[6]);
    if (normals) VertexNormal(base, at, geo, &c[8]);  // side134 (0, 0, 0 without one: the triangle's is used)
    for (int k = 0; k < 3; ++k) {
      c[k] = VertexFloat(base, at + static_cast<uint32_t>(geo.position) + 4 * k, geo.endian);
      if (!std::isfinite(c[k]) || (i < 16 && (c[k] < low[k] - pad || c[k] > high[k] + pad))) {
        why = "positions outside the box";
        return 0;
      }
    }
    const uint32_t color = second_layer ? SecondLayerWeight(base, at, geo) : white;
    std::memcpy(&c[7], &color, 4);
  }
  // the indices, in the byte order that keeps them all inside the vertices
  std::vector<uint16_t> indices(geo.index_count);
  for (int order = 0; order < 2; ++order) {
    bool fits = true;
    for (uint32_t i = 0; i < geo.index_count && fits; ++i) {
      const uint8_t* p = GpuPointer(base, geo.indices + (geo.index_start + i) * geo.index_size);
      uint32_t value;
      if (geo.index_size == 4) {
        value = order == 0 ? (uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3])
                           : (uint32_t{p[3]} << 24 | uint32_t{p[2]} << 16 | uint32_t{p[1]} << 8 | p[0]);
      } else {
        value = order == 0 ? (uint32_t{p[0]} << 8 | p[1]) : (uint32_t{p[1]} << 8 | p[0]);
      }
      fits = value < geo.vertex_count;
      indices[i] = static_cast<uint16_t>(value);
    }
    if (fits) {
      const uint32_t triangles = geo.index_count / 3 * 3;
      mesh = ngl_vk_mesh(corners.data(), geo.vertex_count, indices.data(), triangles);
      ++g_frame_made.meshes;  // [rexglue-native] 2026-11-09
      if (!mesh) {
        why = "ngl_vk has no room";
        g_meshes_full = true;
      }
      return mesh;
    }
  }
  why = "indices past the vertices";
  return 0;
}

// Skinned surfaces (their declaration has BLENDINDICES as UBYTE4 and BLENDWEIGHT as UBYTE4N, run side18): each of a
// vertex's four index bytes names a palette slot and the weight byte in the same place its share; slot s is the
// skeleton's bone given by the 16-bit number at surface +0x70 + 2s; the object holds the bones' matrices (+0xA4 their
// count, +0xA8 the 4x4s, row-vector, 64 bytes each: each bone's pose in the object's space, run side19). A vertex is
// in the model's bind space, so it goes through the bone's inverse bind pose first, then its pose. The pose changes
// every frame, so a skinned surface's corners are made for this frame only (ngl_vk_frame_corners), its mesh giving the
// triangles.
constexpr uint32_t kSurfaceBones = 0x70;
constexpr uint32_t kObjectBoneCount = 0xA4;
constexpr uint32_t kObjectBones = 0xA8;
constexpr uint32_t kMaxSkinnedCorners = 65536;
// The skeleton (run side21): object +0x80 is the model, model +0x10 its skeleton {name, bone count, records}. A bone's
// record is 0x90 bytes: its bind pose against its parent's (+0x00), its inverse bind pose, the model's space to the
// bone's (+0x40), its name (+0x80) and its parent's number (+0x84, -1 for the root). Each pose is four rows (x, y, z
// and the translation) of three floats and a padding word. Checked: the inverse of a bone's +0x40 is its +0x00 times
// its parent's bind pose.
constexpr uint32_t kObjectModel = 0x80;
constexpr uint32_t kModelSkeleton = 0x10;
constexpr uint32_t kSkeletonBoneCount = 0x04;
constexpr uint32_t kSkeletonBones = 0x08;
constexpr uint32_t kBoneRecord = 0x90;
constexpr uint32_t kBoneInverseBind = 0x40;
constexpr bool kSkin = true;

uint32_t U16(uint8_t* base, uint32_t at) { return uint32_t{base[at]} << 8 | base[at + 1]; }

// The skinned position of the vertex at `at` (its own position p), or false when none of its bones is usable.
// `remap`: the slot names a bone through the surface's list (else the slot is the bone itself). side134: with n and
// n_out, its normal n turned by the same bones (their rotations only), as Big Red One's skinning does
// (bro/src/ngl/ngl_mesh.c, ngl_mesh_skinned_corners) and the arms' vertex shader (c[17+a0]..c[19+a0], dp3).
bool SkinPosition(uint8_t* base, uint32_t surface, uint32_t object, const Geometry& geo, uint32_t at, const float p[3],
                  bool remap, float out[3], const float* n = nullptr, float* n_out = nullptr) {
  const uint32_t posed = g::ReadU32(base, object + kObjectBoneCount), bones = g::ReadU32(base, object + kObjectBones);
  if (!Physical(bones) || posed == 0 || posed > 512) return false;
  const uint32_t model = g::ReadU32(base, object + kObjectModel);
  const uint32_t skeleton = Readable(model) ? g::ReadU32(base, model + kModelSkeleton) : 0;
  if (!Readable(skeleton)) return false;
  // side150: the bones both the object poses and the skeleton describes. The soldiers of mayenne and chambois pose 96
  // and their skeleton describes 92 (their vertices use up to 91); all of them were left in their bind pose, which
  // lost the torso of the soldier nearest the camera.
  const uint32_t count = std::min(posed, g::ReadU32(base, skeleton + kSkeletonBoneCount));
  if (count == 0) return false;
  const uint32_t records = g::ReadU32(base, skeleton + kSkeletonBones);
  if (!Readable(records)) return false;
  const uint32_t slots = GpuU32(base, at + static_cast<uint32_t>(geo.blend_indices));
  const uint32_t weights = GpuU32(base, at + static_cast<uint32_t>(geo.blend_weights));
  float sum[3] = {0.0f, 0.0f, 0.0f}, turned[3] = {0.0f, 0.0f, 0.0f}, total = 0.0f;
  for (int k = 0; k < 4; ++k) {
    const float weight = static_cast<float>((weights >> (8 * k)) & 255u);
    if (weight == 0.0f) continue;
    const uint32_t slot = (slots >> (8 * k)) & 255u;
    const uint32_t bone = remap ? U16(base, surface + kSurfaceBones + 2 * slot) : slot;
    if (bone >= count) continue;
    const uint32_t inverse = records + bone * kBoneRecord + kBoneInverseBind;
    float v[3], w[3] = {0.0f, 0.0f, 0.0f};  // the vertex and its normal in the bone's space
    for (int c = 0; c < 3; ++c) {
      const float r0 = g::ReadF32(base, inverse + 4 * c), r1 = g::ReadF32(base, inverse + 16 + 4 * c),
                  r2 = g::ReadF32(base, inverse + 32 + 4 * c);
      v[c] = p[0] * r0 + p[1] * r1 + p[2] * r2 + g::ReadF32(base, inverse + 48 + 4 * c);
      if (n) w[c] = n[0] * r0 + n[1] * r1 + n[2] * r2;
    }
    float m[16];
    for (uint32_t e = 0; e < 16; ++e) m[e] = g::ReadF32(base, bones + bone * 64 + 4 * e);
    for (int c = 0; c < 3; ++c) {
      sum[c] += weight * (v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + m[12 + c]);
      turned[c] += weight * (w[0] * m[c] + w[1] * m[4 + c] + w[2] * m[8 + c]);
    }
    total += weight;
  }
  if (total <= 0.0f) return false;
  for (int c = 0; c < 3; ++c) out[c] = sum[c] / total;
  if (n && n_out) {  // ngl_vk's shader normalizes it; a bad one leaves 0, 0, 0 (the triangle's normal)
    const bool finite = std::isfinite(turned[0]) && std::isfinite(turned[1]) && std::isfinite(turned[2]);
    for (int c = 0; c < 3; ++c) n_out[c] = finite ? turned[c] / total : 0.0f;
  }
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// side216 [acct2-bro] 2026-11-07: SkinPosition for every corner of one surface, with each bone's two matrices (its
// inverse bind and the object's pose) read from guest memory and multiplied once per surface instead of once per
// corner and weight (28 big-endian reads each: SkinPosition was 13% of the worker at 330 fps). The result is the
// same up to float rounding: out = p x (inverse bind x pose), the two steps folded into one.
class SurfaceSkin {
 public:
  SurfaceSkin(uint8_t* base, uint32_t surface, uint32_t object) : base_(base), surface_(surface) {
    const uint32_t posed = g::ReadU32(base, object + kObjectBoneCount), bones = g::ReadU32(base, object + kObjectBones);
    if (!Physical(bones) || posed == 0 || posed > 512) return;
    const uint32_t model = g::ReadU32(base, object + kObjectModel);
    const uint32_t skeleton = Readable(model) ? g::ReadU32(base, model + kModelSkeleton) : 0;
    if (!Readable(skeleton)) return;
    count_ = std::min(posed, g::ReadU32(base, skeleton + kSkeletonBoneCount));  // side150, as SkinPosition
    records_ = g::ReadU32(base, skeleton + kSkeletonBones);
    if (count_ == 0 || !Readable(records_)) {
      count_ = 0;
      return;
    }
    bones_ = bones;
    matrices_.resize(count_);
    ready_.assign(count_, 0);
  }

  bool ok() const { return count_ != 0; }

  // SkinPosition(..., remap true, ...) for one corner.
  bool Skin(const Geometry& geo, uint32_t at, const float p[3], float out[3], const float* n, float* n_out) {
    return SkinWith(GpuU32(base_, at + static_cast<uint32_t>(geo.blend_indices)),
                    GpuU32(base_, at + static_cast<uint32_t>(geo.blend_weights)), p, out, n, n_out);
  }

  // The same with the corner's bone slots and weights (the two words as GpuU32 reads them) already read.
  bool SkinWith(uint32_t slots, uint32_t weights, const float p[3], float out[3], const float* n, float* n_out) {
    float sum[3] = {0.0f, 0.0f, 0.0f}, turned[3] = {0.0f, 0.0f, 0.0f}, total = 0.0f;
    for (int k = 0; k < 4; ++k) {
      const float weight = static_cast<float>((weights >> (8 * k)) & 255u);
      if (weight == 0.0f) continue;
      const uint32_t slot = (slots >> (8 * k)) & 255u;
      const uint32_t bone = U16(base_, surface_ + kSurfaceBones + 2 * slot);
      if (bone >= count_) continue;
      const Matrix& m = MatrixOf(bone);
      for (int c = 0; c < 3; ++c) {
        sum[c] += weight * (p[0] * m.a[0][c] + p[1] * m.a[1][c] + p[2] * m.a[2][c] + m.b[c]);
        if (n) turned[c] += weight * (n[0] * m.a[0][c] + n[1] * m.a[1][c] + n[2] * m.a[2][c]);
      }
      total += weight;
    }
    if (total <= 0.0f) return false;
    for (int c = 0; c < 3; ++c) out[c] = sum[c] / total;
    if (n && n_out) {
      const bool finite = std::isfinite(turned[0]) && std::isfinite(turned[1]) && std::isfinite(turned[2]);
      for (int c = 0; c < 3; ++c) n_out[c] = finite ? turned[c] / total : 0.0f;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
  }

 private:
  struct Matrix {
    float a[3][3];  // a point's row j to the result's column c
    float b[3];     // the translation
  };

  // The inverse bind (rows r_j at +16 j, its translation t at +48) then the pose m (4 x 4, rows of 4): a[j][c] =
  // sum over k of r_j[k] m[4k + c], b[c] = sum over k of t[k] m[4k + c] + m[12 + c].
  const Matrix& MatrixOf(uint32_t bone) {
    Matrix& out = matrices_[bone];
    if (ready_[bone]) return out;
    const uint32_t inverse = records_ + bone * kBoneRecord + kBoneInverseBind;
    float r[4][3], m[16];
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 3; ++k) r[j][k] = g::ReadF32(base_, inverse + 16 * j + 4 * k);
    for (uint32_t e = 0; e < 16; ++e) m[e] = g::ReadF32(base_, bones_ + bone * 64 + 4 * e);
    for (int c = 0; c < 3; ++c) {
      for (int j = 0; j < 3; ++j) out.a[j][c] = r[j][0] * m[c] + r[j][1] * m[4 + c] + r[j][2] * m[8 + c];
      out.b[c] = r[3][0] * m[c] + r[3][1] * m[4 + c] + r[3][2] * m[8 + c] + m[12 + c];
    }
    ready_[bone] = 1;
    return out;
  }

  uint8_t* base_;
  uint32_t surface_;
  uint32_t bones_ = 0, records_ = 0, count_ = 0;
  std::vector<Matrix> matrices_;
  std::vector<uint8_t> ready_;
};

// side218 [acct2-bro] 2026-11-07: what a skinned surface's corners hold that the bones don't change (the bind-pose
// position and normal, both texture coordinates, the bone slots and weights), decoded from guest memory once and
// kept, so a frame only blends the bones. Kept by surface; decoded again when the surface's vertex range changes or a
// sample of its bytes does (16 bytes of every 8th corner: the game rewrites some vertex buffers between frames).
struct SkinSource {
  uint32_t vertices = 0, start = 0, count = 0, stride = 0;
  uint64_t check = 0;
  bool normals = false;
  std::vector<float> rest;  // 10 a corner: position 3, normal 3, the two texture coordinates 2 + 2
  std::vector<uint8_t> has_normal;
  std::vector<uint32_t> slots, weights;
};
std::unordered_map<uint32_t, SkinSource> g_skin_sources;  // the worker's (or the game thread's) only
constexpr size_t kRestFloats = 10;

uint64_t SkinCheck(uint8_t* base, const Geometry& geo) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a over the sampled words
  for (uint32_t i = 0; i < geo.vertex_count; i += 8) {
    const uint32_t at = geo.vertices + (geo.vertex_start + i) * geo.stride;
    for (uint32_t k = 0; k + 4 <= 16 && k + 4 <= geo.stride; k += 4) {
      h ^= GpuU32(base, at + k);
      h *= 1099511628211ull;
    }
  }
  return h;
}

const SkinSource& SkinSourceFor(uint8_t* base, uint32_t surface, const Geometry& geo, bool normals) {
  SkinSource& src = g_skin_sources[surface];
  const uint64_t check = SkinCheck(base, geo);
  if (src.vertices == geo.vertices && src.start == geo.vertex_start && src.count == geo.vertex_count &&
      src.stride == geo.stride && src.check == check && src.normals == normals && !src.rest.empty())
    return src;
  src.vertices = geo.vertices;
  src.start = geo.vertex_start;
  src.count = geo.vertex_count;
  src.stride = geo.stride;
  src.check = check;
  src.normals = normals;
  src.rest.assign(static_cast<size_t>(geo.vertex_count) * kRestFloats, 0.0f);
  src.has_normal.assign(geo.vertex_count, 0);
  src.slots.resize(geo.vertex_count);
  src.weights.resize(geo.vertex_count);
  for (uint32_t i = 0; i < geo.vertex_count; ++i) {
    const uint32_t at = geo.vertices + (geo.vertex_start + i) * geo.stride;
    float* r = &src.rest[static_cast<size_t>(i) * kRestFloats];
    for (int k = 0; k < 3; ++k) r[k] = VertexFloat(base, at + static_cast<uint32_t>(geo.position) + 4 * k, geo.endian);
    src.has_normal[i] = normals && VertexNormal(base, at, geo, &r[3]);  // side134
    VertexUv(base, at, geo, 0, &r[6], &r[7]);
    VertexUv(base, at, geo, 1, &r[8], &r[9]);
    src.slots[i] = GpuU32(base, at + static_cast<uint32_t>(geo.blend_indices));
    src.weights[i] = GpuU32(base, at + static_cast<uint32_t>(geo.blend_weights));
  }
  return src;
}

// This frame's corners of a skinned surface (0 when it isn't skinned or can't be): positions skinned, texture
// coordinates as the mesh's.
thread_local bool t_skin_watch = false;  // rn40o: the caller's draw is a watched helmet (debug only)

// rn40p: whether a texture's own name holds cod3_debug_ngl_side_watch (as WatchDraw matches; debug only)
bool WatchedTextureName(uint8_t* base, uint32_t texture) {
  const std::string watch = Lower(REXCVAR_GET(cod3_debug_ngl_side_watch));
  if (watch.empty() || !texture || !IsTexture(base, texture)) return false;
  const uint32_t name_at = g::ReadU32(base, texture + kTextureName);
  if (!Pointer(name_at) || !Readable(name_at)) return false;
  return Lower(InlineName(base, name_at + 4)).find(watch) != std::string::npos;
}

// rn42a: what the last SkinnedCorners call made (debug only, read by LogNearSkinned): whether the object's bones were
// found, how many corners fell back to their rest place, and the skinned corners' bounds
thread_local struct {
  bool ok = false;
  uint32_t corners = 0, fallbacks = 0;
  float low[3] = {}, high[3] = {};
} t_skin_made;

uint32_t SkinnedCorners(uint8_t* base, uint32_t surface, uint32_t object, uint64_t frame) {
  t_skin_made = {};
  Geometry geo;
  std::string why;
  if (!ReadGeometry(base, surface, geo, why) || geo.blend_indices < 0 || geo.blend_weights < 0 ||
      geo.vertex_count > kMaxSkinnedCorners) {
    if (t_skin_watch) {  // rn40o
      static std::atomic<int> said{0};
      if (said.fetch_add(1, std::memory_order_relaxed) < 4) {
        REXLOG_INFO("cod3: ngl side: rn40o helmet surface {:08X} of object {:08X} isn't skinned: {}; blend indices "
                    "at {}, weights at {}, {} corners", surface, object, why.empty() ? "-" : why, geo.blend_indices,
                    geo.blend_weights, geo.vertex_count);
      }
    }
    return 0;
  }
  static int probes = 0;
  std::vector<float> corners(static_cast<size_t>(geo.vertex_count) * NGL_VK_CORNER_FLOATS, 0.0f);
  const uint32_t white = 0xFFFFFFFFu;
  const bool normals = REXCVAR_GET(cod3_debug_ngl_side_normals);
  SurfaceSkin skin(base, surface, object);  // side216
  const SkinSource& src = SkinSourceFor(base, surface, geo, normals);  // side218
  for (uint32_t i = 0; i < geo.vertex_count; ++i) {
    const uint32_t at = geo.vertices + (geo.vertex_start + i) * geo.stride;
    float* c = &corners[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    const float* r = &src.rest[static_cast<size_t>(i) * kRestFloats];
    const float* p = r;
    const float* n = &r[3];
    const bool has_normal = src.has_normal[i] != 0;
    if (!skin.ok() || !skin.SkinWith(src.slots[i], src.weights[i], p, c, has_normal ? n : nullptr, &c[8])) {
      std::memcpy(c, p, 3 * sizeof(float));
      if (has_normal) std::memcpy(&c[8], n, 3 * sizeof(float));
      ++t_skin_made.fallbacks;  // rn42a
    }
    for (int k = 0; k < 3; ++k) {  // rn42a
      t_skin_made.low[k] = i == 0 ? c[k] : std::min(t_skin_made.low[k], c[k]);
      t_skin_made.high[k] = i == 0 ? c[k] : std::max(t_skin_made.high[k], c[k]);
    }
    if (i == 0 && probes < 4) {
      ++probes;
      float direct[3] = {0, 0, 0};
      const bool ok = SkinPosition(base, surface, object, geo, at, p, false, direct);
      REXLOG_INFO("cod3: ngl side: frame {}: skinned surface {:08X} corner 0 ({:.1f}, {:.1f}, {:.1f}), slots {:08X} "
                  "weights {:08X}, bones {} at {:08X}: through the list ({:.1f}, {:.1f}, {:.1f}), slot as bone "
                  "{}({:.1f}, {:.1f}, {:.1f}); list {}", frame, surface, p[0], p[1], p[2],
                  g::ReadU32(base, at + static_cast<uint32_t>(geo.blend_indices)),
                  g::ReadU32(base, at + static_cast<uint32_t>(geo.blend_weights)),
                  g::ReadU32(base, object + kObjectBoneCount), g::ReadU32(base, object + kObjectBones), c[0], c[1],
                  c[2], ok ? "" : "(none) ", direct[0], direct[1], direct[2],
                  Words(base, surface + kSurfaceBones, 8));
    }
    std::memcpy(&c[3], &r[6], 4 * sizeof(float));  // side218: both texture coordinates, kept
    std::memcpy(&c[7], &white, 4);
  }
  // [rexglue-native] 2026-11-10 rn40o: rn40n drew the helmets taken as 5931940F174EA055 in magenta and none showed:
  // where their corners go. For a helmet (t_skin_watch, set by the caller), the bounds of the rest corners and of the
  // skinned ones, whether its bones were found, and its counts (debug only, 8 lines).
  if (t_skin_watch) {
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
      float low[2][3], high[2][3];
      for (int k = 0; k < 2; ++k) {
        for (int i = 0; i < 3; ++i) {
          low[k][i] = 1e30f;
          high[k][i] = -1e30f;
        }
      }
      for (uint32_t v = 0; v < geo.vertex_count; ++v) {
        const float* rest = &src.rest[static_cast<size_t>(v) * kRestFloats];
        const float* skinned = &corners[static_cast<size_t>(v) * NGL_VK_CORNER_FLOATS];
        for (int i = 0; i < 3; ++i) {
          low[0][i] = std::min(low[0][i], rest[i]);
          high[0][i] = std::max(high[0][i], rest[i]);
          low[1][i] = std::min(low[1][i], skinned[i]);
          high[1][i] = std::max(high[1][i], skinned[i]);
        }
      }
      REXLOG_INFO("cod3: ngl side: rn40o helmet surface {:08X} of object {:08X} (frame {}): {} corners, {} indices, "
                  "bones found {}; rest from {:.1f} {:.1f} {:.1f} to {:.1f} {:.1f} {:.1f}; skinned from {:.1f} {:.1f} "
                  "{:.1f} to {:.1f} {:.1f} {:.1f}",
                  surface, object, frame, geo.vertex_count, geo.index_count, skin.ok() ? 1 : 0, low[0][0], low[0][1],
                  low[0][2], high[0][0], high[0][1], high[0][2], low[1][0], low[1][1], low[1][2], high[1][0],
                  high[1][1], high[1][2]);
    }
  }
  t_skin_made.ok = skin.ok();  // rn42a
  t_skin_made.corners = geo.vertex_count;
  return ngl_vk_frame_corners(corners.data(), geo.vertex_count);
}

// rn42a (owner 09:03: on St-Bert "that burnt slab ... was the bottom of the car/tank we're in. Also the NPC poses are
// wrong when there are in that same car/tank"): each skinned surface of an object near the eye (its origin under 1000
// units deep in the view), on the frames saved from 1350 to 2400, once a surface each 300 frames, up to 120 lines:
// its color map, the object's place, whether its bones were found, the corners left at rest, the bones posed (and of
// those, how many collapsed to under 0.001 scale or aren't numbers: how a hidden part might be posed), and where the
// skinned corners went in the object's space (debug only)
void LogNearSkinned(uint8_t* base, uint32_t object, uint32_t surface, uint32_t color, const ngl_vk_mesh_draw& d,
                    const float* view_projection, uint64_t frame) {
  if (g_quiet || frame < 1350 || frame > 2400 || !REXCVAR_GET(cod3_debug_ngl_side)) return;
  float clip[4] = {};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 3; ++row) clip[column] += d.world[12 + row] * view_projection[4 * row + column];
    clip[column] += view_projection[12 + column];
  }
  if (!(clip[3] > -200.0f && clip[3] < 1000.0f)) return;
  static std::mutex mutex;
  static std::vector<uint64_t> seen;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const uint64_t key = uint64_t{surface} << 16 | (frame / 300);
    if (seen.size() >= 120 || std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
  }
  const uint32_t name_at = IsTexture(base, color) ? g::ReadU32(base, color + kTextureName) : 0;
  const std::string name = Pointer(name_at) && Readable(name_at) ? InlineName(base, name_at + 4) : std::string("-");
  const uint32_t posed = g::ReadU32(base, object + kObjectBoneCount), bones = g::ReadU32(base, object + kObjectBones);
  uint32_t flat = 0, odd = 0, first_flat = 0xFFFF;
  if (Physical(bones) && posed <= 512) {
    for (uint32_t b = 0; b < posed; ++b) {
      float scale = 0.0f;
      bool finite = true;
      for (uint32_t e = 0; e < 12; ++e) {
        const float v = g::ReadF32(base, bones + b * 64 + 4 * e);
        finite = finite && std::isfinite(v);
        if (e < 3) scale += v * v;
      }
      if (!finite) {
        ++odd;
      } else if (scale < 1e-6f) {
        if (!flat) first_flat = b;
        ++flat;
      }
    }
  }
  REXLOG_INFO("cod3: ngl side: rn42a near skinned surface {:08X} of object {:08X} (frame {}, {:.0f} deep): color map "
              "{}, at {:.1f} {:.1f} {:.1f}; bones {} ({} posed at {:08X}; {} collapsed, first {}, {} not numbers); {} "
              "of {} corners at rest; skinned from {:.1f} {:.1f} {:.1f} to {:.1f} {:.1f} {:.1f}; surface bones:{}",
              surface, object, frame, clip[3], name, d.world[12], d.world[13], d.world[14],
              t_skin_made.ok ? "found" : "NOT found", posed, bones, flat, first_flat, odd, t_skin_made.fallbacks,
              t_skin_made.corners, t_skin_made.low[0], t_skin_made.low[1], t_skin_made.low[2], t_skin_made.high[0],
              t_skin_made.high[1], t_skin_made.high[2], Words(base, surface + kSurfaceBones, 8));
}

// One corner of a draw followed through: the surface's first vertex, in the world and in clip space, to check the
// matrices against the picture.
void Probe(uint8_t* base, uint64_t frame, uint32_t surface, const ngl_vk_mesh_draw& d) {
  Geometry geo;
  std::string why;
  if (!ReadGeometry(base, surface, geo, why)) return;
  const uint32_t at = geo.vertices + geo.vertex_start * geo.stride + static_cast<uint32_t>(geo.position);
  const float p[4] = {VertexFloat(base, at, geo.endian), VertexFloat(base, at + 4, geo.endian),
                      VertexFloat(base, at + 8, geo.endian), 1.0f};
  float world[4], clip[4];
  for (int column = 0; column < 4; ++column) {
    world[column] = 0.0f;
    for (int row = 0; row < 4; ++row) world[column] += p[row] * d.world[4 * row + column];
  }
  for (int column = 0; column < 4; ++column) {
    clip[column] = 0.0f;
    for (int row = 0; row < 4; ++row) clip[column] += world[row] * d.view_projection[4 * row + column];
  }
  const float w = clip[3] != 0.0f ? clip[3] : 1.0f;
  float u0, v0, u1, v1;
  VertexUv(base, at - static_cast<uint32_t>(geo.position), geo, 0, &u0, &v0);
  VertexUv(base, at - static_cast<uint32_t>(geo.position), geo, 1, &u1, &v1);
  REXLOG_INFO("cod3: ngl side: surface {:08X}: stride {}, position at {}, uv0 at {}{} ({:.3f}, {:.3f}), uv1 at {}{} "
              "({:.3f}, {:.3f}), texture {}", surface, geo.stride, geo.position, geo.uv[0],
              geo.uv_half[0] ? " (16-bit)" : "", u0, v0, geo.uv[1], geo.uv_half[1] ? " (16-bit)" : "", u1, v1,
              d.texture);
  REXLOG_INFO("cod3: ngl side: frame {}: surface {:08X}'s first corner ({:.1f}, {:.1f}, {:.1f}): world ({:.1f}, "
              "{:.1f}, {:.1f}), clip ({:.2f}, {:.2f}, {:.2f}, {:.2f}), on screen ({:.3f}, {:.3f}) depth {:.4f}",
              frame, surface, p[0], p[1], p[2], world[0], world[1], world[2], clip[0], clip[1], clip[2], clip[3],
              clip[0] / w, clip[1] / w, clip[2] / w);
}

// side93: a skinned surface of the front scene (the first-person arms, missing from the native picture): its first
// corner skinned, then through world x view-projection (what ngl_vk is handed) and through the object's own +0x40 (its
// world-view-projection, as the gun's), with the object's position and bone 0's translation, to see which space the
// bones leave a vertex in.
// side95: also once for a skinned surface of the world (a soldier, drawn right), to set its bones beside the arms'.
void ProbeFrontSkinned(uint8_t* base, uint32_t surface, uint32_t object, const ngl_vk_mesh_draw& d, bool front) {
  static std::mutex probed_mutex;
  static std::unordered_set<uint32_t> probed;
  static int world_probed = 0;
  {
    std::lock_guard lock(probed_mutex);
    if (!front && world_probed >= 1) return;
    if (probed.size() >= 6 || !probed.insert(surface).second) return;
    world_probed += !front;
  }
  Geometry geo;
  std::string why;
  if (!ReadGeometry(base, surface, geo, why) || geo.blend_indices < 0 || geo.blend_weights < 0) return;
  const uint32_t at = geo.vertices + geo.vertex_start * geo.stride;
  float p[3], s[3] = {0.0f, 0.0f, 0.0f};
  for (int k = 0; k < 3; ++k) p[k] = VertexFloat(base, at + static_cast<uint32_t>(geo.position) + 4 * k, geo.endian);
  const bool skinned = SkinPosition(base, surface, object, geo, at, p, true, s);
  float wvp[16], own[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += d.world[4 * r + k] * d.view_projection[4 * k + c];
      wvp[4 * r + c] = sum;
      own[4 * r + c] = g::ReadF32(base, object + 0x40 + 4 * static_cast<uint32_t>(4 * r + c));
    }
  }
  auto clip = [&](const float* m, float out[4]) {
    for (int c = 0; c < 4; ++c) out[c] = s[0] * m[c] + s[1] * m[4 + c] + s[2] * m[8 + c] + m[12 + c];
  };
  float a[4], b[4];
  clip(wvp, a);
  clip(own, b);
  const uint32_t bones = g::ReadU32(base, object + kObjectBones);
  float bone0[3] = {0.0f, 0.0f, 0.0f};
  if (Physical(bones)) {
    for (int k = 0; k < 3; ++k) bone0[k] = g::ReadF32(base, bones + 48 + 4 * static_cast<uint32_t>(k));
  }
  auto ndc = [](const float v[4], int i) { return v[3] != 0.0f ? v[i] / v[3] : 0.0f; };
  REXLOG_INFO("cod3: ngl side: {} skinned surface {:08X} of object {:08X}: corner 0 ({:.2f}, {:.2f}, {:.2f}) "
              "skinned{} ({:.2f}, {:.2f}, {:.2f}); object at ({:.1f}, {:.1f}, {:.1f}); bone 0 at ({:.2f}, {:.2f}, "
              "{:.2f}); through world x view-projection: w {:.3f}, on screen ({:.3f}, {:.3f}); through +0x40: w "
              "{:.3f}, on screen ({:.3f}, {:.3f})", front ? "front" : "world", surface, object, p[0], p[1], p[2], skinned ? "" : " (failed)", s[0],
              s[1], s[2], d.world[12], d.world[13], d.world[14], bone0[0], bone0[1], bone0[2], a[3], ndc(a, 0),
              ndc(a, 1), b[3], ndc(b, 0), ndc(b, 1));
  // side94: the numbers themselves, to work out the arms' space offline: the object's matrix, bone 0's pose and its
  // inverse bind pose, and the scene's view-projection, row by row
  auto rows = [&](uint32_t at, int count, int stride_words) {
    std::string text;
    char item[24];
    for (int r = 0; r < count; ++r) {
      for (int c = 0; c < 4; ++c) {
        const uint32_t word = static_cast<uint32_t>(stride_words * r + c);
        std::snprintf(item, sizeof item, " %.4g", g::ReadF32(base, at + 4 * word));
        text += item;
      }
      text += r + 1 < count ? " |" : "";
    }
    return text;
  };
  std::string vp;
  char item[24];
  for (int k = 0; k < 16; ++k) {
    std::snprintf(item, sizeof item, " %.4g%s", d.view_projection[k], k % 4 == 3 && k < 15 ? " |" : "");
    vp += item;
  }
  const uint32_t model = g::ReadU32(base, object + kObjectModel);
  const uint32_t skeleton = Readable(model) ? g::ReadU32(base, model + kModelSkeleton) : 0;
  const uint32_t records = Readable(skeleton) ? g::ReadU32(base, skeleton + kSkeletonBones) : 0;
  REXLOG_INFO("cod3: ngl side: {} skinned object {:08X}: matrix{}; bone 0 pose{}; bone 0 inverse bind{}; "
              "view-projection{}", front ? "front" : "world", object, rows(object, 4, 4), Physical(bones) ? rows(bones, 4, 4) : std::string(" -"),
              Readable(records) ? rows(records + kBoneInverseBind, 4, 4) : std::string(" -"), vp);
}

// ---------------------------------------------------------------------------------------------------------------
// SpeedTree (runs side23-24). A tree node's record (node +0x0C) is {kind, ?, ?, the tree}: kind 0x8207B7E0 draws
// the tree's branches (sub_823F5C80), 0x8207B7F8 its fronds (sub_823F5868), 0x8207B810 its leaves (sub_823F5028,
// not drawn yet). Branches and fronds are each one vertex buffer and, by level of detail, an index buffer of 32-bit
// indices drawn as one triangle strip (sub_820C7818, type 6) whose length is a 16-bit number in the lengths list.
// A vertex starts with a float4 position in the tree's own space (z up); the base texture's u, v are at +32
// (strides 0x74 and 0x4C, from the vertex format globals 0x82A1B8F4 and 0x82A1B8E8). The tree's instances are a list
// from tree +0x10 (next at +0x98); each has its position at +0x10, its turn about z in degrees at +0x20 and its
// scale at +0x24 (sent to the vertex shader; the game picks a level of detail per instance, here it's always 0).
#if defined(COD3_MP)
constexpr uint32_t kTreeBranches = 0x8207A748, kTreeFronds = 0x8207A760, kTreeLeaves = 0x8207A778;  // (1 of 1)
#else
constexpr uint32_t kTreeBranches = 0x8207B7E0, kTreeFronds = 0x8207B7F8, kTreeLeaves = 0x8207B810;
#endif
constexpr uint32_t kTreeRecordTree = 0x0C;
constexpr uint32_t kTreeFirstInstance = 0x10;
constexpr uint32_t kInstancePosition = 0x10, kInstanceTurn = 0x20, kInstanceScale = 0x24, kInstanceNext = 0x98;
constexpr uint32_t kMaxTreeInstances = 1024;
constexpr uint32_t kTreeUv = 32;

struct TreePart {
  const char* name;
  uint32_t buffer, index_buffers, lengths, vertices, stride, texture;
};
// The texture words are a guess to check: branches set tree +0x2B0 and +0x2E8, fronds +0x2CC and +0x2E8.
const TreePart kBranchPart{"branches", 0x1C8, 0x1CC, 0x1D0, 0x54, 0x74, 0x2B0};
const TreePart kFrondPart{"fronds", 0x1D4, 0x1D8, 0x1DC, 0xAC, 0x4C, 0x2CC};
// Leaves: buffer = the list of vertex buffers by level, lengths = the list of level records (see LeafMesh).
const TreePart kLeafPart{"leaves", 0x1E0, 0, 0xF8, 0, 0x6C, 0x2CC};

// The heap's virtual pages, where the trees keep their D3D buffer headers: followed only where the game's own draw
// follows them.
bool Heap(uint32_t address) { return !(address & 3) && address >= 0x40000000u && address < 0x80000000u; }

std::unordered_map<uint64_t, uint32_t> g_tree_meshes;   // tree << 32 | part's buffer word -> mesh (0: not drawable)
std::unordered_map<uint32_t, uint32_t> g_d3d_textures;  // D3D texture -> ngl_vk texture (0: not drawable)

// An ngl_vk texture for a word that is an NGL texture or a bare D3D texture (fetch constant at +0x1C), else 0.
uint32_t AnyTextureFor(uint8_t* base, uint32_t word) {
  if (IsTexture(base, word)) return TextureFor(base, word);
  if (!(Readable(word) || Heap(word)) || (g::ReadU32(base, word + kFetchConstant) & 3) != 2) return 0;
  const auto found = g_d3d_textures.find(word);
  if (found != g_d3d_textures.end()) return found->second;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0, id = 0;
  std::string why;
  if (Decode(base, word, bgra, width, height, why)) {
    id = ngl_vk_texture(bgra.data(), width, height);
    if (id && HasCutOut(bgra)) g_cut_out.insert(id);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    char name[64];
    std::snprintf(name, sizeof name, "ngl-side-d3d-%08X.ppm", word);
    SavePpm(ScreenshotFolder() / name, bgra.data(), width, height);
  }
  REXLOG_INFO("cod3: ngl side: D3D texture {:08X}: {}x{} {}", word, width, height, id ? "made" : why.c_str());
  g_d3d_textures[word] = id;
  return id;
}

uint32_t TreeMesh(uint8_t* base, uint32_t tree, const TreePart& part, std::string& why) {
  const uint64_t key = static_cast<uint64_t>(tree) << 32 | part.buffer;
  const auto found = g_tree_meshes.find(key);
  if (found != g_tree_meshes.end()) {
    if (!found->second) why = "left out before";
    return found->second;
  }
  uint32_t& mesh = g_tree_meshes[key];
  const uint32_t buffer = g::ReadU32(base, tree + part.buffer);
  const uint32_t index_buffers = g::ReadU32(base, tree + part.index_buffers);
  const uint32_t lengths = g::ReadU32(base, tree + part.lengths);
  const uint32_t count = g::ReadU32(base, tree + part.vertices);
  if (!Heap(buffer) || !Readable(index_buffers) || !Readable(lengths) || count < 3 || count > 65535) {
    why = "no buffers";
    return 0;
  }
  const uint32_t vertices = GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & ~3u);
  const uint32_t first = g::ReadU32(base, index_buffers);
  if (!Physical(vertices) || !Heap(first)) {
    why = "buffers outside memory";
    return 0;
  }
  const uint32_t indices = GuestAddress(g::ReadU32(base, first + 0x18));
  const uint32_t index_bytes = g::ReadU32(base, first + 0x1C);
  const uint32_t length = U16(base, lengths);
  if (!Physical(indices) || length < 3 || length * 4 > index_bytes) {
    why = "a strip longer than its index buffer";
    return 0;
  }
  std::vector<float> corners(static_cast<size_t>(count) * NGL_VK_CORNER_FLOATS, 0.0f);
  const uint32_t white = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = vertices + i * part.stride;
    float* c = &corners[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    for (int k = 0; k < 3; ++k) c[k] = VertexFloat(base, at + 4 * k, 2);  // big-endian floats (side138: GPU data)
    c[3] = VertexFloat(base, at + kTreeUv, 2);
    c[4] = VertexFloat(base, at + kTreeUv + 4, 2);
    if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) {
      why = "positions not numbers";
      return 0;
    }
    std::memcpy(&c[7], &white, 4);
  }
  std::vector<uint16_t> list;  // the strip as a triangle list, its joining (degenerate) triangles left out
  list.reserve(static_cast<size_t>(length) * 3);
  for (uint32_t k = 0; k + 2 < length; ++k) {
    uint32_t a = GpuU32(base, indices + 4 * k), b = GpuU32(base, indices + 4 * k + 4);
    const uint32_t c = GpuU32(base, indices + 4 * k + 8);
    if (a >= count || b >= count || c >= count) {
      why = "indices past the vertices";
      return 0;
    }
    if (a == b || b == c || a == c) continue;
    if (k & 1) std::swap(a, b);
    list.push_back(static_cast<uint16_t>(a));
    list.push_back(static_cast<uint16_t>(b));
    list.push_back(static_cast<uint16_t>(c));
  }
  mesh = ngl_vk_mesh(corners.data(), count, list.data(), static_cast<uint32_t>(list.size()));
  if (!mesh) why = "ngl_vk has no room";
  static int shown = 0;
  if (shown < 6) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: tree {:08X} {}: {} vertices, strip of {} -> {} triangles, mesh {}", tree, part.name,
                count, length, list.size() / 3, mesh);
  }
  return mesh;
}

// A tree's leaves (side27). The game draws a level's leaves (a 72-byte record in the list at tree +0xF8: +4 its
// vertex buffer's number in the list at tree +0x1E0, +8 its leaf count) as six vertices a leaf (sub_820C78D0,
// a triangle list), turned to face the camera in the vertex shader. A leaf vertex (stride 0x6C) holds the leaf's
// center (float4 at 0, the tree's space), the atlas u, v and the corner's number 0-3 (at +32), and the leaf's width
// and height (at +48). Corner 0 is the top right, 1 top left, 2 bottom left, 3 bottom right. Here each leaf is two
// fixed crossed cards (in the x-z and y-z planes, both faces), so one mesh serves every instance and every camera.
uint32_t LeafMesh(uint8_t* base, uint32_t tree, std::string& why) {
  const uint64_t key = static_cast<uint64_t>(tree) << 32 | kLeafPart.buffer;
  const auto found = g_tree_meshes.find(key);
  if (found != g_tree_meshes.end()) {
    if (!found->second) why = "left out before";
    return found->second;
  }
  uint32_t& mesh = g_tree_meshes[key];
  const uint32_t levels = g::ReadU32(base, tree + kLeafPart.lengths);
  const uint32_t buffers = g::ReadU32(base, tree + kLeafPart.buffer);
  if (!Readable(levels) || !Readable(buffers)) {
    why = "no leaf levels";
    return 0;
  }
  const uint32_t number = g::ReadU32(base, levels + 4), leaves = g::ReadU32(base, levels + 8);
  const uint32_t buffer = number < 8 ? g::ReadU32(base, buffers + 4 * number) : 0;
  if (!Heap(buffer) || leaves == 0 || leaves > 4096) {
    why = "no leaf buffer";
    return 0;
  }
  const uint32_t vertices = GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & ~3u);
  if (!Physical(vertices)) {
    why = "leaf buffer outside memory";
    return 0;
  }
  static const float kCorner[4][2] = {{0.5f, 0.5f}, {-0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};
  std::vector<float> corners;
  std::vector<uint16_t> list;
  corners.reserve(static_cast<size_t>(leaves) * 8 * NGL_VK_CORNER_FLOATS);
  const uint32_t white = 0xFFFFFFFFu;
  const bool leaf_light = REXCVAR_GET(cod3_debug_ngl_side_leaf_light);  // rn41x
  for (uint32_t leaf = 0; leaf < leaves && leaf < 65536 / 8; ++leaf) {
    float u[4] = {0, 0, 0, 0}, v[4] = {0, 0, 0, 0}, center[3] = {0, 0, 0}, size[2] = {0, 0};
    // rn41x: the leaf's normal (the float3 at +96) and dimming (the float at +76: the w of the float4 at +64, which
    // 51732ACF1B6A87C5 multiplies its lights by), the same in its six vertices
    float normal[3] = {0, 0, 0}, dim = 1.0f;
    bool seen[4] = {false, false, false, false};
    for (uint32_t k = 0; k < 6; ++k) {
      const uint32_t at = vertices + (leaf * 6 + k) * kLeafPart.stride;
      const int corner = static_cast<int>(g::ReadF32(base, at + 40));
      if (corner < 0 || corner > 3) continue;
      seen[corner] = true;
      u[corner] = g::ReadF32(base, at + 32);
      v[corner] = g::ReadF32(base, at + 36);
      if (k == 0) {
        for (int c = 0; c < 3; ++c) center[c] = g::ReadF32(base, at + 4 * c);
        size[0] = g::ReadF32(base, at + 48);
        size[1] = g::ReadF32(base, at + 52);
        if (leaf_light) {
          for (int c = 0; c < 3; ++c) normal[c] = g::ReadF32(base, at + 96 + 4 * c);
          dim = g::ReadF32(base, at + 76);
        }
      }
    }
    uint32_t color = white;  // rn41x: the dimming in red, green and blue (mesh.frag reads the red), opaque
    if (leaf_light) {
      const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
      if (std::isfinite(length) && length > 1e-4f) {
        for (float& n : normal) n /= length;
      } else {
        normal[0] = normal[1] = 0.0f;
        normal[2] = 1.0f;  // none read: up
      }
      const uint32_t byte = UnitByte(std::isfinite(dim) ? dim : 1.0f);
      color = 0xFF000000u | byte << 16 | byte << 8 | byte;
      static int sampled = 0;
      if (sampled < 12 && leaf < 3) {
        ++sampled;
        REXLOG_INFO("cod3: ngl side: rn41x tree {:08X} leaf {}: normal ({:.3f} {:.3f} {:.3f}), dimming {:.4f}", tree,
                    leaf, normal[0], normal[1], normal[2], dim);
      }
    }
    if (!(seen[0] && seen[1] && seen[2] && seen[3]) || !std::isfinite(center[0] + center[1] + center[2]) ||
        !(size[0] > 0.0f && size[0] < 10000.0f && size[1] > 0.0f && size[1] < 10000.0f)) {
      continue;
    }
    for (int plane = 0; plane < 2; ++plane) {  // the x-z card, then the y-z card
      const uint16_t first = static_cast<uint16_t>(corners.size() / NGL_VK_CORNER_FLOATS);
      for (int corner = 0; corner < 4; ++corner) {
        float c[NGL_VK_CORNER_FLOATS] = {};
        const float across = kCorner[corner][0] * size[0], up = kCorner[corner][1] * size[1];
        c[0] = center[0] + (plane == 0 ? across : 0.0f);
        c[1] = center[1] + (plane == 1 ? across : 0.0f);
        c[2] = center[2] + up;
        c[3] = u[corner];
        c[4] = v[corner];
        std::memcpy(&c[7], &color, 4);
        if (leaf_light) {  // rn41x: the leaf's own normal on both cards and both faces, as the game's (camera-facing)
          for (int k = 0; k < 3; ++k) c[8 + k] = normal[k];
        }
        corners.insert(corners.end(), c, c + NGL_VK_CORNER_FLOATS);
      }
      static const uint16_t kFaces[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};  // both faces
      for (const uint16_t f : kFaces) list.push_back(static_cast<uint16_t>(first + f));
    }
  }
  const uint32_t count = static_cast<uint32_t>(corners.size() / NGL_VK_CORNER_FLOATS);
  if (count == 0) {
    why = "no leaves read";
    return 0;
  }
  mesh = ngl_vk_mesh(corners.data(), count, list.data(), static_cast<uint32_t>(list.size()));
  if (!mesh) why = "ngl_vk has no room";
  static int shown = 0;
  if (shown < 4) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: tree {:08X} leaves: level 0 buffer {} with {} leaves -> {} cards, mesh {}", tree,
                number, leaves, count / 4, mesh);
  }
  return mesh;
}

// The frame's trees (branches, fronds and leaves) as ngl_vk draws: every instance of each tree drawn.
// The grass (side59), drawn by ngl_vk's translation of the game's own grass shaders (ngl_vk/shaders/grass.vert,
// grass.frag; NOTES 2026-11-05 16:02). The strip of blades is made once from the grass manager's vertex and index
// buffers (Direct3D's: the vertex buffer's fetch constant at +0x18/+0x1C, the index buffer's data at +0x18, its size at
// +0x1C; big-endian, turned around here). A frame's constants: c12..c15 the meshes' view-projection (as Direct3D's
// columns), c16..c51 what NGL's pass sends (its compiled list at +0x80: one {base, offset} a vector; the list at
// +0x140 goes with another vertex shader, one without c44..c47), c80..c82 and b0 what 0x823F6DF0 sets from the
// manager's parameters; a patch's c84/c85 and its count of indices as 0x823F6DF0 and 0x823F1C70 work them out
// (cod3mp's 0x824101C8 and 0x8240B6E8).
constexpr uint32_t kGrassVertexBuffer = 0x04, kGrassVertexCount = 0x24, kGrassIndexBuffer = 0x28;
constexpr uint32_t kGrassIndexMost = 0x2C, kGrassIndexStep = 0x30, kGrassLodScale = 0x34;
constexpr uint32_t kGrassSpacing = 0x4C;  // +0x08 a cell's size (1000), +0x0C a patch's (100)
constexpr uint32_t kGrassHalfPatch = 0x194, kGrassHalfCell = 0x198, kGrassPatchesARow = 0x19C;
constexpr uint32_t kGrassBend = 0xC0;
// 0x8290E9AC is NGL's scene being drawn (set only while the render job draws it, so 0 here): the grass's own
// scene stands for it (+0x420 the time the wind runs on).
#if defined(COD3_MP)
constexpr uint32_t kGrassFade = 0x8268362C;     // c82.x's numerator (40 of 41)
#else
constexpr uint32_t kGrassFade = 0x825CE628;     // c82.x's numerator
#endif
constexpr uint32_t kGrassVertexBytes = NGL_VK_GRASS_VERTEX_BYTES;
constexpr size_t kGrassPatches = 1024;

// Diagnostics: a texture as decoded here, its channels (red, green, blue, alpha: means, lows, highs) and its middle
// texel, to check the grass's inputs against what its shader expects.
std::string TexelStats(uint8_t* base, uint32_t texture) {
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!Pointer(texture) || !Decode(base, g::ReadU32(base, texture + kTextureDirect3D), bgra, width, height, why)) {
    return "not decoded (" + why + ")";
  }
  double sum[4] = {};
  int low[4] = {255, 255, 255, 255}, high[4] = {};
  const size_t texels = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < texels; ++i) {
    const uint8_t* p = &bgra[i * 4];
    const int rgba[4] = {p[2], p[1], p[0], p[3]};
    for (int k = 0; k < 4; ++k) {
      sum[k] += rgba[k];
      low[k] = std::min(low[k], rgba[k]);
      high[k] = std::max(high[k], rgba[k]);
    }
  }
  const uint8_t* m = &bgra[(static_cast<size_t>(height / 2) * width + width / 2) * 4];
  char text[240];
  std::snprintf(text, sizeof text,
                "%ux%u, means %.0f %.0f %.0f %.0f, lows %d %d %d %d, highs %d %d %d %d, middle %d %d %d %d", width,
                height, sum[0] / texels, sum[1] / texels, sum[2] / texels, sum[3] / texels, low[0], low[1], low[2],
                low[3], high[0], high[1], high[2], high[3], m[2], m[1], m[0], m[3]);
  return text;
}

// Diagnostics: a texture as an 8x8 grid of mean brightness (0-255, rows top to bottom), to see its picture's shape.
std::string TexelGrid(uint8_t* base, uint32_t texture) {
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!Pointer(texture) || !Decode(base, g::ReadU32(base, texture + kTextureDirect3D), bgra, width, height, why)) {
    return "not decoded (" + why + ")";
  }
  double sum[64] = {};
  int count[64] = {};
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = &bgra[(static_cast<size_t>(y) * width + x) * 4];
      const int cell = static_cast<int>(y * 8 / height) * 8 + static_cast<int>(x * 8 / width);
      sum[cell] += 0.3 * p[2] + 0.59 * p[1] + 0.11 * p[0];
      ++count[cell];
    }
  }
  std::string text;
  char item[8];
  for (int cell = 0; cell < 64; ++cell) {
    std::snprintf(item, sizeof item, "%s%.0f", cell % 8 ? " " : " | ", count[cell] ? sum[cell] / count[cell] : 0.0);
    text += item;
  }
  return text;
}

struct GrassFrame {
  std::vector<float> constants;  // c0..c95
  uint32_t bend = 0, blade = 0;
  std::vector<ngl_vk_grass_patch> patches;
  // [rexglue-native] 2026-11-09: split screen: the part of the picture the grass's scene has (SplitScissor; 0 x 0: the
  // whole picture). rn6's 4-player mp_merv: one player's nearest blades covered the next player's quarter.
  ngl_vk_mesh_draw part{};
};

void ShowGrassInputs(uint8_t* base, const GrassFrame& frame, uint32_t scene) {
  REXLOG_INFO("cod3: ngl side: grass inputs: scene {:08X} time {} (current scene {:08X}); ours:{}", scene,
              g::ReadF32(base, scene + 0x420), g::ReadU32(base, kCurrentScene),
              RegistersText(frame.constants.data(), 12, 4) + RegistersText(frame.constants.data(), 80, 3));
  if (frame.patches.empty()) return;
  const ngl_vk_grass_patch& p = frame.patches.front();
  REXLOG_INFO("cod3: ngl side: grass inputs: first patch c84 ({} {} {} {}) c85 ({} {} {} {}), {} indices", p.place[0],
              p.place[1], p.place[2], p.place[3], p.place[4], p.place[5], p.place[6], p.place[7], p.index_count);
}

// A manager parameter's first float (its value at +0x14).
float GrassParameter(uint8_t* base, uint32_t manager, uint32_t at) {
  const uint32_t parameter = g::ReadU32(base, manager + at);
  if (!GrassReadable(parameter)) return 0.0f;
  const uint32_t value = g::ReadU32(base, parameter + 0x14);
  return GrassReadable(value) ? g::ReadF32(base, value) : 0.0f;
}

int g_grass_mesh = 0;  // 0 not tried, 1 made, -1 couldn't be

// Diagnostics (side72): bytes this side made, saved beside the screenshots, to set beside the game's own
// (ngl-side-grass-vb95.bin) on the PC: side71's blades came out 5-10 times wider than grass.vert's math on the game's
// buffers says.
void SaveHostBytes(const void* data, size_t bytes, const std::string& name) {
  const std::filesystem::path path = ScreenshotFolder() / name;
  std::ofstream out(path, std::ios::binary);
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
  REXLOG_INFO("cod3: ngl side: grass: saved {} bytes to {}", bytes, path.string());
}

// The picture a grass texture is decoded to here (width, height, then BGRA), for the same comparison.
void SaveGrassPicture(uint8_t* base, uint32_t texture, const std::string& name) {
  std::vector<uint8_t> bgra;
  uint32_t size[2] = {0, 0};
  std::string why;
  if (!Pointer(texture) || !Decode(base, g::ReadU32(base, texture + kTextureDirect3D), bgra, size[0], size[1], why)) {
    REXLOG_INFO("cod3: ngl side: grass: {} not saved ({})", name, why);
    return;
  }
  std::vector<uint8_t> file(sizeof size + bgra.size());
  std::memcpy(file.data(), size, sizeof size);
  std::memcpy(file.data() + sizeof size, bgra.data(), bgra.size());
  SaveHostBytes(file.data(), file.size(), name);
}

void MakeGrassMesh(uint8_t* base, uint32_t manager) {
  if (g_grass_mesh) return;
  const uint64_t drawn = g_grass_fetch.load(std::memory_order_relaxed);
  if (!drawn) return;  // the game hasn't drawn its grass yet: tried again next frame
  g_grass_mesh = -1;
  const uint32_t vertex_buffer = g::ReadU32(base, manager + kGrassVertexBuffer);
  const uint32_t index_buffer = g::ReadU32(base, manager + kGrassIndexBuffer);
  const uint32_t vertices = g::ReadU32(base, manager + kGrassVertexCount);
  const uint32_t indices = g::ReadU32(base, manager + kGrassIndexMost);
  if (!GrassReadable(vertex_buffer) || !GrassReadable(index_buffer) || !vertices || vertices > 65535 || !indices ||
      indices > (1u << 20)) {
    REXLOG_INFO("cod3: ngl side: grass: no blades ({:08X}, {:08X}, {} vertices, {} indices)", vertex_buffer,
                index_buffer, vertices, indices);
    return;
  }
  const uint32_t fetch = static_cast<uint32_t>(drawn >> 32), size = static_cast<uint32_t>(drawn);
  const uint32_t vertex_data = GuestAddress(fetch & 0x1FFFFFFCu);
  const uint32_t index_data = GuestAddress(g::ReadU32(base, index_buffer + 0x18));
  REXLOG_INFO("cod3: ngl side: grass: vertices at {:08X}, as the game's draw fetches them; the manager's vertex buffer "
              "object {:08X}: {}",
              vertex_data, vertex_buffer, Words(base, vertex_buffer, 12));
  // The drawn address is kept nowhere a native-only path could read it so far: not within two pointers of the
  // manager, not in the heap pages of the game's own grass buffer objects, not in the image (side79-81,
  // acct2-bro/NOTES.md 18:47). So it is taken from the game's grass draw.
  if ((fetch & 3) != 3 || ((size >> 2) & 0xFFFFFF) * 4 < vertices * kGrassVertexBytes || !Physical(vertex_data) ||
      (g::ReadU32(base, index_buffer) & 0x80000000u) || g::ReadU32(base, index_buffer + 0x1C) < indices * 2 ||
      !Physical(index_data)) {
    REXLOG_INFO("cod3: ngl side: grass: blades not read (vertex fetch {:08X} {:08X}, index buffer {})", fetch, size,
                Words(base, index_buffer, 8));
    return;
  }
  std::vector<uint8_t> blades(static_cast<size_t>(vertices) * kGrassVertexBytes);
  for (uint32_t i = 0; i < vertices; ++i) {
    const uint32_t at = vertex_data + kGrassVertexBytes * i;
    const uint32_t root = GpuU32(base, at), shape = GpuU32(base, at + 4), variety = GpuU32(base, at + 8);
    uint8_t* out = blades.data() + static_cast<size_t>(kGrassVertexBytes) * i;
    for (int k = 0; k < 4; ++k) {  // 16_16: x in the low half; 8_8_8_8: x in the low byte
      out[k] = static_cast<uint8_t>(root >> (8 * k));
      out[4 + k] = static_cast<uint8_t>(shape >> (8 * k));
      out[8 + k] = static_cast<uint8_t>(variety >> (8 * k));
    }
  }
  std::vector<uint16_t> strip(indices);
  for (uint32_t i = 0; i < indices; ++i) strip[i] = GpuU16(base, index_data + 2 * i);
  SaveHostBytes(blades.data(), blades.size(), "ngl-side-grass-ours-vb.bin");  // diagnostics (side72)
  SaveHostBytes(strip.data(), strip.size() * sizeof strip[0], "ngl-side-grass-ours-ib.bin");
  if (ngl_vk_grass_mesh(blades.data(), vertices, strip.data(), indices) != 0) {
    REXLOG_INFO("cod3: ngl side: grass: ngl_vk took no blades (no grass pipeline?)");
    return;
  }
  g_grass_mesh = 1;
  REXLOG_INFO("cod3: ngl side: grass: {} vertices and {} indices made", vertices, indices);
}

GrassFrame GrassFor(uint8_t* base, const Census& census) {
  GrassFrame frame;
  if (census.grass.empty()) return frame;
  const Solid& node = census.grass.front();
  const uint32_t object = g::ReadU32(base, node.node + 0x0C), set = g::ReadU32(base, node.node + 0x10);
  const uint32_t manager = GrassReadable(object) ? g::ReadU32(base, object) : 0;
  const uint32_t ngl = GrassReadable(manager) ? g::ReadU32(base, manager) : 0;
  const uint32_t globals = node.scene;
  float view_projection[16];
  bool reversed = false;
  const bool camera = SceneCamera(base, node.scene, view_projection, &reversed);
  if (!GrassReadable(ngl) || !GrassReadable(set) || !GrassReadable(globals) || !camera) {
    static bool told = false;
    if (!told) {
      told = true;
      REXLOG_INFO("cod3: ngl side: grass: not drawn (material {:08X}, NGL material {:08X}, patch set {:08X}, scene "
                  "{:08X} {})",
                  manager, ngl, set, globals, camera ? "with a camera" : "without a perspective camera");
    }
    return frame;
  }
  MakeGrassMesh(base, manager);
  if (g_grass_mesh != 1) return frame;
  SplitScissor(base, node.scene, frame.part);  // [rexglue-native] 2026-11-09: kept to its player's part
  float* c = (frame.constants = std::vector<float>(NGL_VK_GRASS_CONSTANTS * 4, 0.0f)).data();
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) c[(12 + column) * 4 + row] = view_projection[4 * row + column];
  }
  // c16..c51 as NGL's pass sends them (0x82155EC0)
  static int grass_frames = 0;  // diagnostics: on the second frame drawn here, these registers and where each came from
  const bool show_list = ++grass_frames == 2;
  std::string list_text;
  const uint32_t holder = g::ReadU32(base, ngl + 0x0C);
  const uint32_t pass = GrassReadable(holder) ? g::ReadU32(base, holder + 0x0C) : 0;
  const uint32_t compiled = GrassReadable(pass) ? g::ReadU32(base, pass + 0xDC) : 0;
  if (GrassReadable(compiled)) {
    const uint32_t list = compiled + 0x80;
    const uint32_t bases[8] = {0, g::ReadU32(base, ngl + 0x1C), globals, kPassBase3, kPassBase4, 0, kPassBase6,
                               g::ReadU32(base, kPassBase7Pointer)};
    const uint32_t count = g::ReadU32(base, list + 0x08), rows = g::ReadU32(base, list + 0x20);
    const uint32_t first = (g::ReadU32(base, list + 0x1C) & 0x7FF) / 4;
    if (show_list) {
      char item[96];
      std::snprintf(item, sizeof item, " list %08X: %u rows from c%u, header %08X %08X;", list, count, first,
                    g::ReadU32(base, list + 0x18), g::ReadU32(base, list + 0x1C));
      list_text += item;
    }
    for (uint32_t k = 0; k < count && first + k < NGL_VK_GRASS_CONSTANTS && GrassReadable(rows); ++k) {
      const uint32_t slot = g::ReadU32(base, rows + 8 * k) / 4, offset = g::ReadU32(base, rows + 8 * k + 4);
      const uint32_t value = ((slot < 8 ? bases[slot] : 0) + offset) & ~15u;
      if (show_list) {
        char item[64];
        std::snprintf(item, sizeof item, " c%u slot %u+%X (%08X)", first + k, slot, offset, value);
        list_text += item;
      }
      if (!GrassReadable(value)) continue;
      for (uint32_t j = 0; j < 4; ++j) c[(first + k) * 4 + j] = g::ReadF32(base, value + 4 * j);
    }
  }
  if (show_list) {
    REXLOG_INFO("cod3: ngl side: grass constants from NGL's pass {:08X}:{}", pass, list_text);
    REXLOG_INFO("cod3: ngl side: grass constants, ours:{}{}{}{}{}", RegistersText(c, 24, 6), RegistersText(c, 34, 5),
                RegistersText(c, 40, 1), RegistersText(c, 42, 6), RegistersText(c, 50, 1));
  }
  // c80..c82 and b0 (0x823F6DF0)
  auto parameter = [&](uint32_t at) { return GrassParameter(base, manager, at); };
  const float c80[4] = {parameter(0x8C) * g::ReadF32(base, globals + 0x420), parameter(0x84) - parameter(0x80),
                        parameter(0x98) - 1.0f, parameter(0xA4) - 1.0f};
  const float c81[4] = {parameter(0x70) * 0.5f, parameter(0x74) * 0.5f, parameter(0x90) * 0.5f,
                        parameter(0x94) * 0.5f};
  const float range = parameter(0x90) - parameter(0x94);
  const float c82[4] = {range != 0.0f ? g::ReadF32(base, kGrassFade) / range : 0.0f, 0.7f, 0.0f, 0.0f};
  std::memcpy(c + 80 * 4, c80, sizeof c80);
  std::memcpy(c + 81 * 4, c81, sizeof c81);
  std::memcpy(c + 82 * 4, c82, sizeof c82);
  frame.bend = parameter(kGrassBend) != 0.0f;
  // the blade texture: the material's "grass_bladetexture"; and its "time", which the wind runs on when the scene
  // here has none (the game's c80.x is the time of the scene being drawn, a copy this side never sees)
  float time = -1.0f;
  int listed = 0;
  for (uint32_t p = g::ReadU32(base, ngl + 0x30); GrassReadable(p) && listed < 64; p = g::ReadU32(base, p + 0x0C)) {
    ++listed;
    const uint32_t name = g::ReadU32(base, p + 0x04), value = g::ReadU32(base, p + 0x14);
    if (!GrassReadable(name) || !GrassReadable(value)) continue;
    const char* text = reinterpret_cast<const char*>(base + name + 4);
    if (std::string(text, strnlen(text, 32)) == "time") time = g::ReadF32(base, value);
    if (std::string(text, strnlen(text, 32)) == "grass_bladetexture") {
      const uint32_t blade = g::ReadU32(base, value);
      frame.blade = TextureFor(base, blade);
      static bool blade_shown = false;  // diagnostics: the blade's picture as decoded here
      if (!blade_shown) {
        blade_shown = true;
        REXLOG_INFO("cod3: ngl side: grass blade texture {}: {}; brightness{}", TextureText(base, blade),
                    TexelStats(base, blade), TexelGrid(base, blade));
      }
    }
  }
  if (c80[0] == 0.0f && time > 0.0f) c[80 * 4] = parameter(0x8C) * time;
  // the patches
  const uint32_t spacing = g::ReadU32(base, manager + kGrassSpacing);
  const int per_row = static_cast<int>(g::ReadU32(base, manager + kGrassPatchesARow));
  if (!GrassReadable(spacing) || per_row <= 0) return frame;
  const float cell_size = g::ReadF32(base, spacing + 8), patch_size = g::ReadF32(base, spacing + 0x0C);
  const float half_patch = g::ReadF32(base, manager + kGrassHalfPatch);
  const float half_cell = g::ReadF32(base, manager + kGrassHalfCell);
  const uint32_t step = g::ReadU32(base, manager + kGrassIndexStep), most = g::ReadU32(base, manager + kGrassIndexMost);
  const float lod_scale = g::ReadF32(base, manager + kGrassLodScale);
  const uint32_t cells = g::ReadU32(base, set), list = g::ReadU32(base, set + 4);
  const uint32_t count = g::ReadU32(base, set + 0x0C) & 0xFFFF;
  uint32_t first_color = 0, first_heights = 0;  // diagnostics (side72): the first patch's two textures
  for (uint32_t k = 0; k < count && frame.patches.size() < kGrassPatches && GrassReadable(list); ++k) {
    const uint32_t patch = g::ReadU32(base, list + 4 * k);
    if (!GrassReadable(patch)) continue;
    const uint32_t cell = cells + 8 * (g::ReadU32(base, patch + 0x0C) >> 16);
    const uint32_t kind = g::ReadU32(base, cell), data = g::ReadU32(base, cell + 4);
    const uint32_t descriptor = GrassReadable(kind) ? g::ReadU32(base, kind) : 0;
    if (!GrassReadable(descriptor) || !GrassReadable(data)) continue;
    const uint32_t color = g::ReadU32(base, data + 0x0C), heights = g::ReadU32(base, data + 0x08);
    if (!color || !heights) continue;
    const int cells_a_row = static_cast<int>(g::ReadU32(base, descriptor + 0x1C));
    const uint32_t cell_at = g::ReadU32(base, data), patch_at = g::ReadU32(base, patch);
    if (cells_a_row <= 0 || !GrassReadable(cell_at) || !GrassReadable(patch_at)) continue;
    const int cell_index = static_cast<int>(g::ReadU32(base, cell_at));
    const int patch_index = static_cast<int>(g::ReadU32(base, patch_at));
    const float cell_y = static_cast<float>(cell_index / cells_a_row) * cell_size + g::ReadF32(base, descriptor + 8);
    const float cell_x = static_cast<float>(cell_index % cells_a_row) * cell_size + g::ReadF32(base, descriptor + 4);
    const float y = static_cast<float>(patch_index / per_row) * patch_size + cell_y + half_patch;
    const float x = static_cast<float>(patch_index % per_row) * patch_size + cell_x + half_patch;
    ngl_vk_grass_patch out{};
    out.color_texture = TextureFor(base, color);
    out.cell_texture = TextureFor(base, heights);
    static int textures_shown = 0;  // diagnostics: the first two cells' textures as decoded here
    static uint32_t last_shown = 0;
    if (textures_shown < 2 && data != last_shown) {
      ++textures_shown;
      last_shown = data;
      REXLOG_INFO("cod3: ngl side: grass cell {:08X}: color texture {}: {}", data, TextureText(base, color),
                  TexelStats(base, color));
      REXLOG_INFO("cod3: ngl side: grass cell {:08X}: cell texture {}: {}", data, TextureText(base, heights),
                  TexelStats(base, heights));
    }
    out.place[0] = ((x - cell_x) - half_cell) * 0.001f + 0.5f;
    out.place[1] = -(((y - cell_y) - half_cell) * 0.001f - 0.5f);
    out.place[2] = x;
    out.place[3] = y;
    const float low = g::ReadF32(base, descriptor + 0x0C);
    out.place[4] = low;
    out.place[5] = g::ReadF32(base, descriptor + 0x18) - low;
    const uint32_t wanted = step * static_cast<uint32_t>(std::max(0.0f, lod_scale * g::ReadF32(base, patch + 4)));
    out.index_count = wanted < step ? step : wanted > most ? most : wanted;
    if (frame.patches.empty()) {
      first_color = color;
      first_heights = heights;
    }
    frame.patches.push_back(out);
  }
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_INFO("cod3: ngl side: grass: {} of {} patches, blade texture {}, c80 ({} {} {} {}), c82.x {}, b0 {}",
                frame.patches.size(), count, frame.blade, c80[0], c80[1], c80[2], c80[3], c82[0], frame.bend);
    ShowGrassInputs(base, frame, globals);
    // diagnostics (side72): what ngl_vk is handed, to emulate grass.vert on exactly it on the PC; and the patches'
    // places, to set beside the game's own draws' c84 and c85 (the "grass draw" lines)
    std::vector<float> handed(frame.constants);
    for (size_t p = 0; p < frame.patches.size() && p < 16; ++p) {
      handed.insert(handed.end(), frame.patches[p].place, frame.patches[p].place + 8);
    }
    SaveHostBytes(handed.data(), handed.size() * sizeof(float), "ngl-side-grass-ours-constants.bin");
    SaveGrassPicture(base, first_color, "ngl-side-grass-color0.bgra");
    SaveGrassPicture(base, first_heights, "ngl-side-grass-cell0.bgra");
    std::string places;
    for (size_t p = 0; p < frame.patches.size() && p < 12; ++p) {
      const float* q = frame.patches[p].place;
      char item[96];
      std::snprintf(item, sizeof item, " (%g %g %g %g / %g %g)", q[0], q[1], q[2], q[3], q[4], q[5]);
      places += item;
    }
    REXLOG_INFO("cod3: ngl side: grass: first patches' c84 / c85:{}", places);
  }
  return frame;
}

void SetGrass(const GrassFrame& grass) {
  if (grass.constants.empty() || grass.patches.empty()) return;
  ngl_vk_set_grass_scissor(grass.part.scissor_x, grass.part.scissor_y, grass.part.scissor_width,
                           grass.part.scissor_height);  // [rexglue-native] 2026-11-09
  ngl_vk_set_grass(grass.constants.data(), grass.bend, grass.blade, grass.patches.data(),
                   static_cast<uint32_t>(grass.patches.size()));
}

// side208: why draws are left out (and what the drawn ones are), counted only on the frames that log it (g_quiet is
// false on the dump frames). Every other frame skips the string work: the main thread's profile (side199) had
// ngl_side at 28-37% of its time, much of it a label printed and hashed for each draw every frame.
class Reasons {
 public:
  explicit Reasons(bool on) : on_(on) {}
  bool on() const { return on_; }
  int& operator[](const char* why) { return on_ ? counts_[why] : sink_; }
  int& operator[](const std::string& why) { return on_ ? counts_[why] : sink_; }
  const std::unordered_map<std::string, int>& counts() const { return counts_; }

 private:
  bool on_;
  int sink_ = 0;  // what the quiet frames count into, never read
  std::unordered_map<std::string, int> counts_;
};

// rn41y: the light the game hands a tree's leaves, read from the leaf effect's own parameters as the game last set
// them (dev/waw-decomp/cod3/decomp). sub_823F0128 keeps the parameters' handles on the tree: +0x360
// g_vMaterialAmbient (c17), +0x364 g_vMaterialDiffuse (c45), +0x36C g_vLightDir0 (c60), +0x370 g_vLightDir1 (c61),
// +0x374 g_vLightColor0 (c58), +0x378 g_vLightColor1 (c59). In the lit pass ([0x8290E9AC]+0x328 set) sub_823F5028
// calls sub_823EF260, which sets the ambient and diffuse from the scene's sun (sub_8213FE78's globals, 0x825A25B0 on;
// tree +0x340 the sun's place, +0x350 its color), and for each instance sub_823F2170: the way toward tree +0x340 (in the
// tree's turned space), tree +0x350 scaled as the sun, and the nearest point light (sub_82150050's list) as the
// second. A vector parameter's value is the float4 at [handle +0x14] (sub_82141CF0). rn41w on St-Bert: c17 (0.479875
// 0.451 0.364375), c45 1, c58 (0.86625 0.77 0.48125), c59 0; natively the sun was (0.584 0.568 0.428), ambient 0.39.
struct LeafLight {
  float ambient[4] = {}, diffuse[4] = {}, color0[4] = {}, color1[4] = {}, sun_place[4] = {};
  bool place = false;  // the sun's place (tree +0x340) read
};

bool LeafParameter(uint8_t* base, uint32_t tree, uint32_t handle_at, float out[4]) {
  const uint32_t handle = g::ReadU32(base, tree + handle_at);
  if (!(Readable(handle) || Heap(handle))) return false;
  const uint32_t value = g::ReadU32(base, handle + 0x14) & ~0xFu;
  if (!(Readable(value) || Heap(value))) return false;
  for (int i = 0; i < 4; ++i) {
    out[i] = g::ReadF32(base, value + 4 * i);
    if (!std::isfinite(out[i]) || std::fabs(out[i]) > 1000.0f) return false;
  }
  return true;
}

bool ReadLeafLight(uint8_t* base, uint32_t tree, LeafLight& light) {
  if (!LeafParameter(base, tree, 0x360, light.ambient) || !LeafParameter(base, tree, 0x364, light.diffuse) ||
      !LeafParameter(base, tree, 0x374, light.color0) || !LeafParameter(base, tree, 0x378, light.color1)) {
    return false;
  }
  for (int i = 0; i < 4; ++i) light.sun_place[i] = g::ReadF32(base, tree + 0x340 + 4 * i);
  light.place = std::isfinite(light.sun_place[0] + light.sun_place[1] + light.sun_place[2]) &&
                std::fabs(light.sun_place[0]) + std::fabs(light.sun_place[1]) + std::fabs(light.sun_place[2]) > 0.0f;
  return true;
}

// rn41x / rn41y: a check on the leaves' light, on the frames saved (a few in all): the game's leaf parameters beside
// the native sun (g_sun)
void LogLeafLight(uint32_t scene, uint32_t tree, bool read, const LeafLight& l) {
  static int logged = 0;
  if (g_quiet || logged >= 6) return;
  ++logged;
  REXLOG_INFO("cod3: ngl side: rn41y leaves of tree {:08X} (scene {:08X}): game's {}: ambient ({:.5f} {:.5f} {:.5f} "
              "{:.5f}) diffuse ({:.4f} {:.4f} {:.4f}) sun ({:.5f} {:.5f} {:.5f}) second ({:.4f} {:.4f} {:.4f}) sun's "
              "place ({:.1f} {:.1f} {:.1f}); native sun ok {} color ({:.5f} {:.5f} {:.5f}) ambient ({:.5f} {:.5f} "
              "{:.5f}) way ({:.4f} {:.4f} {:.4f})",
              tree, scene, read ? "read" : "NOT read", l.ambient[0], l.ambient[1], l.ambient[2], l.ambient[3],
              l.diffuse[0], l.diffuse[1], l.diffuse[2], l.color0[0], l.color0[1], l.color0[2], l.color1[0],
              l.color1[1], l.color1[2], l.sun_place[0], l.sun_place[1], l.sun_place[2], g_sun.ok, g_sun.color[0],
              g_sun.color[1], g_sun.color[2], g_sun.ambient[0], g_sun.ambient[1], g_sun.ambient[2],
              g_sun.direction[0], g_sun.direction[1], g_sun.direction[2]);
}

void TreeDraws(uint8_t* base, const Census& census, std::vector<ngl_vk_mesh_draw>& draws, Reasons& left_out) {
  // a tree's part is drawn once a frame, all its instances; [rexglue-native] 2026-11-09: once in each split-screen
  // player's view, so each gets its trees (it was once a frame in all: only the first view had them); every
  // whole-screen scene shares view 0, so one player and the campaign stay as they were
  std::unordered_map<uint32_t, std::unordered_set<uint64_t>> done;
  std::unordered_map<uint32_t, std::pair<bool, std::array<float, 16>>> cameras;
  for (const Solid& t : census.trees) {
    const uint32_t record = g::ReadU32(base, t.node + 0x0C);
    if (!Readable(record)) continue;
    const uint32_t kind = g::ReadU32(base, record);
    const TreePart* part = kind == kTreeBranches ? &kBranchPart
                           : kind == kTreeFronds ? &kFrondPart
                           : kind == kTreeLeaves ? &kLeafPart
                                                 : nullptr;
    if (!part) {  // others, by kind
      if (left_out.on()) {
        char label[64];
        std::snprintf(label, sizeof label, "(tree nodes of kind %08X not drawn yet)", kind);
        left_out[label]++;
      }
      continue;
    }
    const uint32_t tree = g::ReadU32(base, record + kTreeRecordTree);
    ngl_vk_mesh_draw split_part{};
    const uint32_t view = SplitScissor(base, t.scene, split_part) ? t.scene : 0;
    if (!Readable(tree) || done[view].count(static_cast<uint64_t>(tree) << 32 | part->buffer)) continue;
    auto camera = cameras.find(t.scene);
    if (camera == cameras.end()) {
      std::array<float, 16> vp{};
      bool reversed = false;
      const bool ok = SceneCamera(base, t.scene, vp.data(), &reversed);
      camera = cameras.emplace(t.scene, std::make_pair(ok, vp)).first;
    }
    if (!camera->second.first) {  // the shadow map's scene (side25: every tree part is in one first)
      left_out["(trees: in a scene without a perspective camera)"]++;
      continue;
    }
    done[view].insert(static_cast<uint64_t>(tree) << 32 | part->buffer);
    std::string why;
    const uint32_t mesh = kind == kTreeLeaves ? LeafMesh(base, tree, why) : TreeMesh(base, tree, *part, why);
    if (!mesh) {
      left_out["(trees: " + why + ")"]++;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = mesh;
    d.texture = AnyTextureFor(base, g::ReadU32(base, tree + part->texture));
    d.tint = kind == kTreeBranches ? 0x5A4632u : 0x3C6432u;
    if (kind == kTreeLeaves && d.texture) g_cut_out.insert(d.texture);  // the leaf atlas is always cut out
    d.shading = NGL_VK_SHADE_LIGHT;
    // rn41x: the leaves by the game's light (cod3_debug_ngl_side_leaf_light): the scene's ambient plus the sun alone,
    // times each leaf's dimming (its corners' red), unshadowed and not saturated before the map (6189945E43D7FBC8:
    // saturate(map x (light + ambient)))
    // rn41y: with the game's own leaf light where its parameters read (ReadLeafLight): the ambient c17, the sun c45 x
    // c58, and each instance's way from the sun's place (tree +0x340), as sub_823F2170; else the native sun as rn41x.
    // The second light (c59, a point light nearby) isn't drawn yet (0 on St-Bert).
    LeafLight leaf;
    bool leaf_sun_place = false;
    if (kind == kTreeLeaves && d.texture && REXCVAR_GET(cod3_debug_ngl_side_leaf_light)) {
      const bool read = ReadLeafLight(base, tree, leaf);
      if (read || g_sun.ok) {
        LightWith(d, g_sun);  // the way, and the colors when the game's aren't read
        d.lit = 1;
        d.extra_light_count = 0;
        d.light_saturate = 0;
        d.tint = NGL_VK_TINT_LIGHT_DIMMED;
        if (read) {
          for (int i = 0; i < 3; ++i) {
            d.ambient[i] = leaf.ambient[i];
            d.light_color[i] = leaf.diffuse[i] * leaf.color0[i];
          }
          leaf_sun_place = leaf.place;
        }
      }
      LogLeafLight(t.scene, tree, read, leaf);
      // rn42y: how many trees' leaves took the game's leaf light and how many the last node's sun (g_sun), counted on
      // the frames that log the census (the live frames saved among them)
      left_out[read ? "(trees: rn42y leaves by the game's leaf light)" : "(trees: rn42y leaves by the last sun)"]++;
    }
    d.alpha = g_cut_out.count(d.texture) ? NGL_VK_ALPHA_TEST : NGL_VK_ALPHA_OPAQUE;
    FogWith(base, t.scene, d);
    std::memcpy(d.view_projection, camera->second.second.data(), sizeof d.view_projection);
    SplitScissor(base, t.scene, d);  // [rexglue-native] 2026-11-09: kept in its player's part of the screen
    uint32_t instance = g::ReadU32(base, tree + kTreeFirstInstance);
    for (uint32_t n = 0; Physical(instance) && n < kMaxTreeInstances && draws.size() < kMaxDraws; ++n) {
      const float x = g::ReadF32(base, instance + kInstancePosition);
      const float y = g::ReadF32(base, instance + kInstancePosition + 4);
      const float z = g::ReadF32(base, instance + kInstancePosition + 8);
      const float turn = g::ReadF32(base, instance + kInstanceTurn) * 3.14159265f / 180.0f;
      const float scale = g::ReadF32(base, instance + kInstanceScale);
      instance = g::ReadU32(base, instance + kInstanceNext);
      if (!std::isfinite(x + y + z + turn + scale) || scale <= 0.0f || scale > 100.0f) {
        left_out["(trees: an instance not sane)"]++;
        continue;
      }
      const float c = std::cos(turn) * scale, s = std::sin(turn) * scale;
      const float world[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, scale, 0, x, y, z, 1};
      std::memcpy(d.world, world, sizeof world);
      if (leaf_sun_place) {  // rn41y: the sun's light travels from its place to this instance (sub_823F2170)
        const float way[3] = {x - leaf.sun_place[0], y - leaf.sun_place[1], z - leaf.sun_place[2]};
        const float length = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
        if (length > 1e-3f) {
          for (int i = 0; i < 3; ++i) d.light_direction[i] = way[i] / length;
        }
      }
      draws.push_back(d);
      left_out[kind == kTreeBranches ? "(trees: branches drawn)"
               : kind == kTreeFronds ? "(trees: fronds drawn)"
                                     : "(trees: leaves drawn)"]++;
    }
  }
}

// side155: which maps each material has and which way the native draw takes it (side154: the jeeps came out white and
// the ground lacks its second diffuse map), each material once from frame 1500 on, up to kMaterialsSurveyed: each
// sampler's name, its texture's own name, and the texture's base and format (to find the material's pixel shader among
// the shader census's, which lists the textures bound)
constexpr int kMaterialsSurveyed = 160;

void SurveyMaterial(uint8_t* base, uint32_t material, uint32_t object, bool skinned, const char* path, uint64_t frame) {
  static std::mutex survey_mutex;
  static std::unordered_set<uint32_t> seen;
  if (frame < 1500 || !REXCVAR_GET(cod3_debug_ngl_side) || !Pointer(material)) return;
  {
    std::lock_guard lock(survey_mutex);
    if (seen.size() >= kMaterialsSurveyed || !seen.insert(material).second) return;
  }
  const uint32_t list = g::ReadU32(base, material + kMaterialSamplers);
  if (!Readable(list)) return;
  std::string text;
  for (uint32_t k = 0; k < kMaxSamplers; ++k) {
    const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
    const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
    if (!Pointer(parameter)) break;
    const uint32_t word1 = g::ReadU32(base, entry + 12);  // the fetch constant's dword 1: base and format
    // [rexglue-native] 2026-11-09 rn21: and how the sampler reads it (side185's fields, NoteFetchKind): dword 0's
    // signs (bits 2-9, 2 bits a channel, 3 gamma) and dword 3's exponent (bits 13-18, signed: what it fetches times
    // 2^this). The soldiers' fabric shader (3BA30872B3955C23) multiplies its diffuse by 0.51; is the diffuse map
    // fetched doubled to make up for it?
    const uint32_t word0 = g::ReadU32(base, entry + 8), word3 = g::ReadU32(base, entry + 20);
    const int exponent_field = static_cast<int>((word3 >> 13) & 0x3F);
    const int exponent = exponent_field >= 32 ? exponent_field - 64 : exponent_field;
    std::string own = "-";
    if (IsTexture(base, texture)) {
      const uint32_t name = g::ReadU32(base, texture + kTextureName);
      if (Pointer(name) && Readable(name)) own = InlineName(base, name + 4);
    }
    const std::string variable = parameter == kLightMapParameter ? "(lightmap)" : SamplerTextureName(base, parameter);
    char item[256];
    std::snprintf(item, sizeof item, "%s%u %s \"%s\" %08X/%u signs %02X exp %d", text.empty() ? "" : ", ", k,
                  variable.c_str(), own.c_str(), word1 & ~0xFFFu, word1 & 0x3F, (word0 >> 2) & 0xFF, exponent);
    text += item;
  }
  REXLOG_INFO("cod3: ngl side: survey frame {}: material {:08X} of object {:08X} ({}, {}): {}", frame, material, object,
              skinned ? "skinned" : "rigid", path, text);
}

// [rexglue-native] 2026-11-10 rn40w: a material's samplers in their order, each as its texture variable's name (as
// SurveyMaterial lists them, names only; debug logs). rn40v: the helmets' pass binding (1D4725CEF0551EBB's) is also
// ADC02DCE4FDC9253's (papajack's and bangboom's bodies), a different shader (a woven swatch folded into the normal):
// what the materials hold may tell them apart.
std::string SamplerNames(uint8_t* base, uint32_t material) {
  const uint32_t list = Pointer(material) && Readable(material + kMaterialSamplers)
                            ? g::ReadU32(base, material + kMaterialSamplers)
                            : 0;
  if (!Readable(list)) return "-";
  std::string text;
  for (uint32_t k = 0; k < kMaxSamplers; ++k) {
    const uint32_t parameter = g::ReadU32(base, list + kSamplerFirst + kSamplerBytes * k);
    if (!Pointer(parameter)) break;
    if (!text.empty()) text += " ";
    text += parameter == kLightMapParameter ? "(lightmap)" : SamplerTextureName(base, parameter);
  }
  return text;
}

// [rexglue-native] 2026-11-09 rn35: a watched color map's native draw (cod3_debug_ngl_side_watch), once per color map
// from frame 2900: its material's maps as SurveyMaterial lists them, how the native side draws it, the light it gives
// it (before the sun's shadow is set for the frame, ShadowFor), and its pass's binding (LogPassBinding). Its physical
// base is kept for the host-draw log's rn35 lines. Debug only.
constexpr uint64_t kWatchFrom = 2900;
// rn40d: a color map whose name doesn't hold the text (or that has none, as the Polish infantry's helmets) is watched
// by its normal map's name instead: that map's base is kept, so the host-draw log gives every draw that reads it.
void WatchDraw(uint8_t* base, uint32_t node, uint32_t object, uint32_t material, uint32_t color, uint32_t normal,
               bool skinned, bool front, const ngl_vk_mesh_draw& d, uint64_t frame) {
  std::string watch;
  {
    static std::mutex mutex;
    static uint64_t read_at = ~uint64_t{0};
    static std::string text;
    std::lock_guard<std::mutex> lock(mutex);
    if (read_at != frame) {
      read_at = frame;
      text = Lower(REXCVAR_GET(cod3_debug_ngl_side_watch));
    }
    watch = text;
  }
  if (watch.empty()) return;
  const auto own_name = [&](uint32_t texture) -> std::string {
    if (!texture || !IsTexture(base, texture)) return "";
    const uint32_t name_at = g::ReadU32(base, texture + kTextureName);
    return Pointer(name_at) && Readable(name_at) ? InlineName(base, name_at + 4) : std::string();
  };
  std::string name = own_name(color);
  uint32_t watched = color;
  if (name.empty() || Lower(name).find(watch) == std::string::npos) {
    name = own_name(normal);
    watched = normal;
  }
  if (name.empty() || Lower(name).find(watch) == std::string::npos) return;
  // [rexglue-native] 2026-11-10 rn40i: a watched helmet (5931940F174EA055): its object and the light kept for it, and
  // the object watched for the game's passes and draws (WatchLightObject)
  if (d.lens_cube && d.lens_opaque && REXCVAR_GET(cod3_debug_ngl_side) && Readable(object + 0x38)) {
    float at[3];
    for (int i = 0; i < 3; ++i) at[i] = g::ReadF32(base, object + 0x30 + 4 * i);
    if (WatchedLightObject(object, nullptr, nullptr) != 1) {
      WatchLightObject(object, at);
      std::string kept = "none";
      {
        std::lock_guard<std::mutex> lock(g_model_light_mutex);
        const auto it = g_model_lights.find(object);
        if (it != g_model_lights.end()) {
          kept = std::to_string(static_cast<int64_t>(frame) - static_cast<int64_t>(it->second.frame)) +
                 " frames old, row xx red " + std::to_string(it->second.rows[0][0]);
        }
      }
      REXLOG_INFO("cod3: ngl side: rn40i watching helmet object {:08X} \"{}\" (frame {}) at {:.1f} {:.1f} {:.1f}: kept "
                  "light {}", object, name, frame, at[0], at[1], at[2], kept);
      // rn40k: the way to its sun (lens_sun_place) in the mesh's own space (the world's rows are the mesh's axes in
      // the world), the rows' lengths, and the draw's world translation (to see whether N.L can be right)
      float way[3] = {d.lens_sun_place[0] - d.world[12], d.lens_sun_place[1] - d.world[13],
                      d.lens_sun_place[2] - d.world[14]};
      const float way_length = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
      float mesh_way[3] = {}, lengths[3] = {};
      for (int i = 0; i < 3; ++i) {
        lengths[i] = std::sqrt(d.world[4 * i] * d.world[4 * i] + d.world[4 * i + 1] * d.world[4 * i + 1] +
                               d.world[4 * i + 2] * d.world[4 * i + 2]);
        if (way_length > 0.0f && lengths[i] > 0.0f) {
          mesh_way[i] = (d.world[4 * i] * way[0] + d.world[4 * i + 1] * way[1] + d.world[4 * i + 2] * way[2]) /
                        (way_length * lengths[i]);
        }
      }
      REXLOG_INFO("cod3: ngl side: rn40k helmet object {:08X}: sun at {:.1f} {:.1f} {:.1f}, the draw's world at {:.1f} "
                  "{:.1f} {:.1f}; the way to the sun in the mesh's space {:.3f} {:.3f} {:.3f}; the world's rows' "
                  "lengths {:.3f} {:.3f} {:.3f}; floor {:.3g}, fade {:.3g}, sun color {:.3f} {:.3f} {:.3f}; rows {}",
                  object, d.lens_sun_place[0], d.lens_sun_place[1], d.lens_sun_place[2], d.world[12], d.world[13],
                  d.world[14], mesh_way[0], mesh_way[1], mesh_way[2], lengths[0], lengths[1], lengths[2],
                  d.lens_shadow_floor, d.lens_shadow_fade, d.lens_sun_color[0], d.lens_sun_color[1],
                  d.lens_sun_color[2], d.irradiance_rows);
      // rn40n: what ngl_vk.c's lens_draw checks (a texture, no normal map, environment map or second texture, lit,
      // the scene's light), and the draw's alpha
      REXLOG_INFO("cod3: ngl side: rn40n helmet object {:08X}: lens_cube {}, texture {}, normal_map {}, env_texture {}, "
                  "blend_texture {}, blend_light {}, lit {}, shading {}, alpha {}, show {}",
                  object, d.lens_cube, d.texture, d.normal_map, d.env_texture, d.blend_texture, d.blend_light, d.lit,
                  d.shading, d.alpha, d.lens_show);
    }
  }
  // [rexglue-native] 2026-11-10 rn40q: every watched draw of frames 2900-2903, not only the first of each map (the
  // helmet's own draw may share the head's maps): its object and place, rigid or skinned, how it is drawn, its rows
  if (frame >= 2900 && frame <= 2903 && REXCVAR_GET(cod3_debug_ngl_side)) {
    static std::atomic<int> said{0};
    if (said.fetch_add(1, std::memory_order_relaxed) < 40) {
      const char* kind = d.metal_cube     ? "metal"
                         : d.lens_cube    ? (d.lens_opaque ? "5931940F (eyes)" : "glass with a cube")
                         : d.env_texture  ? "shiny"
                         : d.fabric_mask  ? "fabric"
                         : d.noise_swatch ? "skin"
                         : d.normal_map   ? (d.specular_map ? "bumped with a specular map" : "bumped")
                         : d.lightmap     ? "lightmapped"
                                          : "plain";
      REXLOG_INFO("cod3: ngl side: rn40q draw (frame {}) of \"{}\": object {:08X} at {:.1f} {:.1f} {:.1f} ({}), "
                  "material {:08X}, mesh {}, corners {}, {}, lit {}, rows {} (xx red {:.3f}), normal map {}, "
                  "specular map {}; light {:.3f} {:.3f} {:.3f} traveling {:.3f} {:.3f} {:.3f}, ambient {:.3f}, "
                  "shading {}, texture {}",
                  frame, name, object, d.world[12], d.world[13], d.world[14], skinned ? "skinned" : "rigid", material,
                  d.mesh, d.corners, kind, d.lit, d.irradiance_rows, d.irradiance[0][0], d.normal_map,
                  d.specular_map, d.light_color[0], d.light_color[1], d.light_color[2], d.light_direction[0],
                  d.light_direction[1], d.light_direction[2], d.ambient[0], d.shading, d.texture);
    }
  }
  uint32_t fetch[6];
  if (!ReadFetch(base, g::ReadU32(base, watched + kTextureDirect3D), fetch)) return;
  const uint32_t at = fetch[1] & 0x1FFFF000u;
  if (!KeepWatchedDiffuse(at)) return;
  std::string maps;
  const uint32_t list = Pointer(material) ? g::ReadU32(base, material + kMaterialSamplers) : 0;
  for (uint32_t k = 0; Readable(list) && k < kMaxSamplers; ++k) {
    const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
    const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
    if (!Pointer(parameter)) break;
    std::string own = "-";
    if (IsTexture(base, texture)) {
      const uint32_t own_at = g::ReadU32(base, texture + kTextureName);
      if (Pointer(own_at) && Readable(own_at)) own = InlineName(base, own_at + 4);
    }
    const std::string variable = parameter == kLightMapParameter ? "(lightmap)" : SamplerTextureName(base, parameter);
    char item[256];
    std::snprintf(item, sizeof item, "%s%u %s \"%s\" %08X", maps.empty() ? "" : ", ", k, variable.c_str(),
                  own.c_str(), g::ReadU32(base, entry + 12) & ~0xFFFu);
    maps += item;
  }
  const char* how = d.metal_cube     ? "metal"
                    : d.lens_cube    ? (d.lens_opaque ? "the eyes with a cube" : "glass with a cube")
                    : d.env_texture  ? "shiny"
                    : d.fabric_mask  ? "fabric"
                    : d.detail_map   ? "detail-mapped metal"
                    : d.vehicle_bumped ? "a bumped vehicle part"
                    : d.noise_swatch ? "skin"
                    : d.normal_map   ? (d.specular_map ? "bumped with a specular map" : "bumped")
                    : d.lightmap     ? "lightmapped"
                                     : "plain";
  REXLOG_INFO("cod3: ngl side: rn35 watched draw (frame {}): color map \"{}\" at {:08X}, object {:08X} ({}{}), "
              "material {:08X}: {}", frame, name, at, object, skinned ? "skinned" : "rigid", front ? ", in front" : "",
              material, maps);
  REXLOG_INFO("cod3: ngl side: rn35 watched draw of \"{}\": native {}, shading {}, lit {}, saturate {}; ambient {:.3f} "
              "{:.3f} {:.3f}; light {:.3f} {:.3f} {:.3f} traveling {:.3f} {:.3f} {:.3f}; {} extra; rows {}: xx {:.3f} "
              "{:.3f} {:.3f}, yy {:.3f} {:.3f} {:.3f}, zz {:.3f} {:.3f} {:.3f}, z {:.3f} {:.3f} {:.3f}", name, how,
              d.shading, d.lit, d.light_saturate, d.ambient[0], d.ambient[1], d.ambient[2], d.light_color[0],
              d.light_color[1], d.light_color[2], d.light_direction[0], d.light_direction[1], d.light_direction[2],
              d.extra_light_count, d.irradiance_rows, d.irradiance[0][0], d.irradiance[0][1], d.irradiance[0][2],
              d.irradiance[1][0], d.irradiance[1][1], d.irradiance[1][2], d.irradiance[2][0], d.irradiance[2][1],
              d.irradiance[2][2], d.irradiance[8][0], d.irradiance[8][1], d.irradiance[8][2]);
  // rn40: where the mesh's own origin falls in the picture, through the draw's view-projection (Direct3D's row vectors):
  // 0 to 1 across from the left and down from the top; -1 -1 behind the eye
  float clip[4] = {};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 3; ++row) clip[column] += d.world[12 + row] * d.view_projection[4 * row + column];
    clip[column] += d.view_projection[12 + column];
  }
  const float across = clip[3] > 0.0f ? 0.5f + 0.5f * clip[0] / clip[3] : -1.0f;
  const float down = clip[3] > 0.0f ? 0.5f - 0.5f * clip[1] / clip[3] : -1.0f;
  REXLOG_INFO("cod3: ngl side: rn35 watched draw of \"{}\": {} point light(s): color {:.3f} {:.3f} {:.3f}, 1 / range "
              "{:.6f}; world at {:.1f} {:.1f} {:.1f} (in the picture at {:.3f} {:.3f}); fog {:.0f} to {:.0f} ({:.3f})",
              name, d.point_light_count, d.point_light[0][4], d.point_light[0][5], d.point_light[0][6],
              d.point_light[0][3], d.world[12], d.world[13], d.world[14], across, down, d.fog[0], d.fog[1], d.fog[2]);
  LogPassBinding(base, node, ("watched " + name).c_str());
}

// [rexglue-native] 2026-11-10 rn42t: the lit models' light on the live frames saved (owner 10:29: the grass, bushes
// and other things "flicker with lighting"; rn42s: in The Mace, with the camera still, the sitting soldier and the
// tank on the right went dark, lit, dark, lit on frames in a row). Up to kLiveLightLines near lit models a frame, in
// the census's order: where each one's light came from and what was handed to ngl_vk, so the frames can be set side
// by side. Debug only (cod3_debug_ngl_side_live_shots).
// rn42y: and where the sun came from (sun_source) and whether the model is skinned; 48 lines within 800 units (rn42x's
// 24 within 1500 missed St-Bert's gunner, lit on 1801-1802 and dark on 1803-1806)
constexpr int kLiveLightLines = 48;
constexpr float kLiveLightNear = 800.0f;  // the model's clip w (about its distance from the eye, in game units)
void LogLiveLight(uint64_t frame, uint32_t object, const ngl_vk_mesh_draw& d, const float* view_projection,
                  bool front, const char* source, uint64_t kept, const char* sun_source, bool skinned,
                  const float* block_sun) {
  float clip[4] = {};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 3; ++row) clip[column] += d.world[12 + row] * view_projection[4 * row + column];
    clip[column] += view_projection[12 + column];
  }
  if (!(clip[3] > 0.0f) || clip[3] > kLiveLightNear) return;
  const float across = 0.5f + 0.5f * clip[0] / clip[3], down = 0.5f - 0.5f * clip[1] / clip[3];
  if (!(across > -0.1f && across < 1.1f && down > -0.1f && down < 1.1f)) return;
  static std::mutex mutex;
  static uint64_t lines_frame = 0;
  static int lines = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (lines_frame != frame) {
      lines_frame = frame;
      lines = 0;
    }
    if (lines >= kLiveLightLines) return;
    ++lines;
  }
  size_t objects_kept = 0, cells_kept = 0;
  {
    std::lock_guard<std::mutex> lock(g_model_light_mutex);
    objects_kept = g_model_lights.size();
    cells_kept = g_kept_light_cells.size();
  }
  REXLOG_INFO("cod3: ngl side: rn42t live light (frame {}): object {:08X}{} mesh {} at {:.0f} {:.0f} {:.0f} (picture "
              "{:.2f} {:.2f}, w {:.0f}): {} (kept on frame {}; {} objects' lights kept, {} cells); sun {:.3f} {:.3f} "
              "{:.3f} traveling {:.2f} {:.2f} {:.2f}; ambient {:.3f} {:.3f} {:.3f}; {} rows, xx yy zz red {:.3f} {:.3f} "
              "{:.3f}; {} point light(s), color {:.2f} {:.2f} {:.2f}; shading {}, normal map {}, fabric mask {}; "
              "rn42y: the sun from {}{}; its light block's main light {:.3f} {:.3f} {:.3f}",
              frame, object, front ? " (front scene)" : "", d.mesh, d.world[12], d.world[13], d.world[14], across, down,
              clip[3], source, kept, objects_kept, cells_kept, d.light_color[0], d.light_color[1], d.light_color[2],
              d.light_direction[0], d.light_direction[1], d.light_direction[2], d.ambient[0], d.ambient[1],
              d.ambient[2], d.irradiance_rows, d.irradiance[0][0], d.irradiance[1][0], d.irradiance[2][0],
              d.point_light_count, d.point_light[0][4], d.point_light[0][5], d.point_light[0][6],
              static_cast<int>(d.shading), d.normal_map, d.fabric_mask, sun_source, skinned ? ", skinned" : "",
              block_sun[0], block_sun[1], block_sun[2]);
}

// The frame's generic surfaces as ngl_vk draws, scene by scene; the reasons any are left out, counted.
// side213: appended to draws (the sky's are there already), so the frame's list isn't copied again
void SolidDraws(uint8_t* base, uint64_t frame, const Census& census, std::vector<ngl_vk_mesh_draw>& draws) {
  const size_t first = draws.size();
  draws.reserve(first + census.solids.size() + census.trees.size());
  Reasons left_out(!g_quiet || g_live_shot);  // rn42t: and the live frames saved (debug only)
  struct Camera {
    bool ok, reversed, front, other;
    float view_projection[16];
    bool split;  // [rexglue-native] 2026-11-09: a split-screen player's view, kept in its part (SplitScissor)
    int32_t scissor_x, scissor_y;
    uint32_t scissor_width, scissor_height;
    bool probed;  // [rexglue-native] 2026-11-09 rn6: its first surface followed through (Probe), with the pictures
  };
  std::unordered_map<uint32_t, Camera> cameras;
  static std::unordered_set<uint32_t> scenes_shown;
  // The scene with the most generic surfaces is the world. The first-person gun and arms have a scene of their own
  // whose camera's near plane is far closer (0.05 units, its far 128, against the world's 6 and 6500); the game
  // keeps them in front of the world, but ngl_vk has one depth buffer for everything, so such a scene's depth is
  // squeezed into the nearest 2% (as Big Red One's ngl_present.c does for the scene after its world). It isn't told
  // by order: side45 found the arms in a scene drawn first (order 0, no target of its own).
  std::unordered_map<uint32_t, int> per_scene;
  for (const Solid& s : census.solids) per_scene[s.scene]++;
  uint32_t world_scene = 0;
  int most = 0;
  // [rexglue-native] 2026-11-10 rn43b: with cod3_debug_ngl_side_front_target, a scene drawn into no target (+0x328 0:
  // side83's third scene, the vehicles' treads and soldiers seen from the light, the owner's burnt slab, 11:50) is
  // the world only when no scene has a target: it is never shown, however many surfaces it has
  const bool targets_count = REXCVAR_GET(cod3_debug_ngl_side_front_target);
  for (int pass = 0; pass < 2 && !world_scene; ++pass) {
    for (const auto& [scene, n] : per_scene) {
      if (pass == 0 && targets_count && !g::ReadU32(base, scene + kSceneColorTarget)) continue;
      if (n > most) {
        most = n;
        world_scene = scene;
      }
    }
  }
  const float world_near = world_scene ? NearPlane(base, world_scene) : 0.0f;
  // rn42v: the world's own target, which a scene drawn in front of it must share (cod3_debug_ngl_side_front_target)
  const uint32_t world_target = world_scene ? g::ReadU32(base, world_scene + kSceneColorTarget) : 0;
  {  // side223
    bool reversed = false;
    g_world_camera.ok = world_scene && SceneCamera(base, world_scene, g_world_camera.view_projection, &reversed);
  }
  g_world_depth = WorldDepthOf(base, world_scene);  // side125: for the depth of field's distances
  if (g_world_depth.ok) g_world_depth.reversed = DepthAt(g_world_depth, 1000.0f) < DepthAt(g_world_depth, 10.0f);
  for (const Solid& s : census.solids) {
    auto camera = cameras.find(s.scene);
    if (camera == cameras.end()) {
      Camera c{};
      c.ok = SceneCamera(base, s.scene, c.view_projection, &c.reversed);
      const float near_plane = NearPlane(base, s.scene);
      {  // [rexglue-native] 2026-11-09: split screen (SplitScissor)
        ngl_vk_mesh_draw part{};
        c.split = c.ok && SplitScissor(base, s.scene, part);
        c.scissor_x = part.scissor_x;
        c.scissor_y = part.scissor_y;
        c.scissor_width = part.scissor_width;
        c.scissor_height = part.scissor_height;
      }
      const bool near_as_front = c.ok && s.scene != world_scene && world_near > 0.0f && near_plane > 0.0f &&
                                 near_plane < kFrontNear * world_near;
      // rn42v: and drawn into the world's target (the third scene, without one, isn't the gun's)
      const bool target_as_front = !REXCVAR_GET(cod3_debug_ngl_side_front_target) ||
                                   g::ReadU32(base, s.scene + kSceneColorTarget) == world_target;
      if (near_as_front && !target_as_front) left_out["(rn42v: a near scene not drawn into the world's target)"]++;
      if (near_as_front && target_as_front) {
        for (int row = 0; row < 4; ++row) c.view_projection[4 * row + 2] *= kFrontDepth;
        c.front = true;
        left_out["(scenes in front of the world)"]++;
      } else if (c.ok && s.scene != world_scene && c.split && std::fabs(near_plane - world_near) <= 0.05f * world_near) {
        // [rexglue-native] 2026-11-09: another split-screen player's view of the world (the world's near plane, its
        // own part of the screen): drawn through its own camera. rn2's 4-player mp_merv left these out as side83's
        // third scene, so only one player's view was drawn; side83's scene (near plane 0.43 to 4.6, the whole screen)
        // still is.
        left_out["(split screen: another player's view)"]++;
      } else if (c.ok && s.scene != world_scene) {
        c.other = true;  // side83: neither the world nor the gun's (the third scene of side43-45)
      }
      camera = cameras.emplace(s.scene, c).first;
      {  // [rexglue-native] 2026-11-09 rn6: each split view's part of the picture, with the pictures (rn4's p4side drew
         // only player 1's quarter natively, the others black, though their surfaces were counted as drawn)
        static int split_lines = 0;
        if (!g_quiet && c.split && split_lines < 64) {
          ++split_lines;
          REXLOG_INFO("cod3: ngl side: frame {}: split view: scene {:08X}{}{}, its part x {} y {} {}x{} of the {}x{} "
                      "picture, near {:.2f}", frame, s.scene, s.scene == world_scene ? " (the world's)" : "",
                      c.front ? " (in front)" : c.other ? " (left out)" : "", c.scissor_x, c.scissor_y,
                      c.scissor_width, c.scissor_height, g_target_width, g_target_height, near_plane);
        }
      }
      if (scenes_shown.size() < kShownScenes && scenes_shown.insert(s.scene).second) {
        const uint32_t target = g::ReadU32(base, s.scene + kSceneColorTarget);
        REXLOG_INFO("cod3: ngl side: 3D scene {:08X}: target {:08X} flags {:X}, {} camera{}", s.scene, target,
                    Pointer(target) ? g::ReadU32(base, target + kTargetFlags) : 0,
                    c.ok ? "a perspective" : "not a perspective", c.ok && c.reversed ? ", depth backward" : "");
        for (uint32_t row = 0x100; row < 0x200; row += 0x20) {
          REXLOG_INFO("cod3: ngl side: 3D scene {:08X} +{:03X}: {}", s.scene, row, Words(base, s.scene + row, 8));
        }
      }
    }
    if (!camera->second.ok) {
      left_out["its scene has no perspective camera"]++;
      continue;
    }
    // The third perspective scene (side45: drawn first, no target of its own, near plane 0.43 to 4.6 units, up to 31
    // skinned surfaces): drawn through its own camera it put a soldier's body and a large hand near the bottom of the
    // picture where the game shows none (side76-82). Left out, nothing else visible goes (side83): most likely the
    // pass that draws the soldiers into their shadow texture.
    if (camera->second.other) {
      left_out["(side83: the third perspective scene)"]++;
      continue;
    }
    const uint32_t object = g::ReadU32(base, s.node + kNodeObject);
    const uint32_t surface = g::ReadU32(base, s.node + kNodeSurface);
    if (!Pointer(object) || !Pointer(surface)) {
      left_out["no object or surface"]++;
      continue;
    }
    // rn43a: its pass's values as the game drew it, for the pass readers below (cod3_debug_ngl_side_drawn_values)
    const DrawnScope drawn_scope(object, frame);
    if (g_meshes_full) {
      left_out["ngl_vk has no room"]++;
      continue;
    }
    std::string why;
    const uint32_t mesh = MeshFor(base, surface, why);
    if (!mesh) {
      left_out[why]++;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = mesh;
    const uint32_t material = g::ReadU32(base, s.node + kNodeMaterial);
    d.tint = 0x404040u | (((material * 2654435761u) >> 8) & 0xBFBFBFu);
    Geometry geo;
    std::string unused;
    ReadGeometry(base, surface, geo, unused);
    const bool skinned = geo.blend_weights >= 0 && geo.blend_indices >= 0;
    if (!s.old_class) {  // an old class's node has no shader state and its material no sampler list
      ShowSamplers(base, material, geo.stride, skinned);
      ShowShaderState(base, g::ReadU32(base, s.node + kNodeShaderState));
      HuntFog(base, g::ReadU32(base, s.node + kNodeShaderState));
      HuntLights(base, g::ReadU32(base, s.node + kNodeShaderState));  // side181
    }
    const TextureFrame look_frame = FrameFor(base, s.scene, object);
    Samplers samplers = s.old_class ? Samplers{} : MaterialSamplers(base, material, skinned, look_frame.value);
    if (s.old_class) {
      if (s.old_class == kOldWorld) samplers.lightmap = SecondTexture(base, material);
      left_out[s.old_class == kOldWorld ? "(of those, cdWorld's nodes)" : "(of those, cdSimple's nodes)"]++;
      static int old_shown = 0;
      if (old_shown < 6) {
        ++old_shown;
        REXLOG_INFO("cod3: ngl side: {} node {:08X}: object {:08X}, surface {:08X}, material {:08X}: {} / {}; color "
                    "map {:08X}, lightmap {:08X}; stride {}", s.old_class == kOldWorld ? "cdWorld" : "cdSimple",
                    s.node, object, surface, material, Words(base, material, 8), Words(base, material + 0x20, 8),
                    MaterialTexture(base, material), samplers.lightmap, geo.stride);
      }
    }
    if (samplers.looks) {
      left_out[look_frame.own ? "(of those, a look picked by the object's frame)"
                              : "(of those, a look picked by the scene's frame)"]++;
      static int looks_shown = 0;
      if (looks_shown < 16) {
        ++looks_shown;
        const uint32_t name = g::ReadU32(base, samplers.looks + kTextureName);
        // rn40g: and the picked look's own name
        const uint32_t picked = Readable(samplers.color + kTextureName) ? g::ReadU32(base, samplers.color + kTextureName)
                                                                         : 0;
        REXLOG_INFO("cod3: ngl side: looks {:08X} \"{}\" ({} of them): object {:08X} frame {} ({}) -> {:08X} \"{}\"",
                    samplers.looks, Pointer(name) ? InlineName(base, name + 4) : std::string(),
                    g::ReadU32(base, samplers.looks + kLookCount), object, look_frame.value,
                    look_frame.own ? "its own" : "the scene's", samplers.color,
                    Pointer(picked) && Readable(picked) ? InlineName(base, picked + 4) : std::string("-"));
      }
    }
    if (camera->second.front) {
      std::lock_guard<std::mutex> lock(g_front_mutex);
      if (g_front_objects.size() < 64) g_front_objects.insert(object);
    }
    if (camera->second.front && !skinned) {  // side90: does a rigid gun part hang on a bone? (the gun sits off)
      static int rigid_shown = 0;
      const uint32_t bone_count = g::ReadU32(base, object + kObjectBoneCount);
      if (rigid_shown < 8) {
        ++rigid_shown;
        REXLOG_INFO("cod3: ngl side: front rigid surface {:08X} of object {:08X}: {} bones at {:08X}, model {:08X}; "
                    "surface +60: {} / {}; object +30: {}", surface, object, bone_count,
                    g::ReadU32(base, object + kObjectBones), g::ReadU32(base, object + kObjectModel),
                    Words(base, surface + 0x60, 8), Words(base, surface + 0x80, 8), Words(base, object + 0x30, 8));
      }
    }
    if (camera->second.front) {  // the gun's materials, once each: the texture taken as the color map, and the list
      static std::unordered_set<uint32_t> front_materials;
      if (front_materials.size() < 10 && front_materials.insert(material).second) {
        REXLOG_INFO("cod3: ngl side: front scene {:08X} material {:08X} ({}): color map {:08X}", s.scene, material,
                    skinned ? "skinned" : "rigid", samplers.color);
        ShowSamplers(base, material, geo.stride, skinned, true);
      }
    }
    d.texture = TextureFor(base, samplers.color ? samplers.color : MaterialTexture(base, material));
    d.lightmap = TextureFor(base, samplers.lightmap);
    d.shading = d.lightmap ? NGL_VK_SHADE_LIGHTMAP : NGL_VK_SHADE_LIGHT;
    // side165: lit as the game's lightmapped shaders do (not the old classes': cdWorld's own draw, side90)
    const bool world_lit = d.lightmap && !s.old_class && REXCVAR_GET(cod3_debug_ngl_side_world_light);
    if (d.lightmap && !samplers.color && samplers.env && samplers.normal) {  // side188: water (the puddles)
      {
        std::lock_guard<std::mutex> lock(g_water_mutex);
        if (g_water_objects.size() < 64) g_water_objects.insert(object);
      }
      if (REXCVAR_GET(cod3_debug_ngl_side_water) == 0) {  // side189
        left_out["water (no color map, an environment and a normal map): its shader isn't drawn natively yet"]++;
        continue;
      }
      left_out["(of those, water: no color map, an environment and a normal map)"]++;
    }
    FabricPass fabric_pass_read;  // rn31: kept for the light below
    bool fabric_read = false;
    bool detail_read = false;  // rn36: the vehicles' detail-mapped metal, its sun as the pass binds it
    bool vehicle_read = false;  // rn39: the vehicles' bumped parts, the same
    bool helmet_read = false;   // rn40t: the soldiers' helmets, the same
    HelmetPass helmet_pass;     // rn40t
    PlainHeadPass plain_head_pass;  // rn40y
    // [rexglue-native] 2026-11-09 rn34: the guns' metal (CFC45995D1294CCE, ReadMetalPass): an environment cube with a
    // normal and a specular map, in a pass bound as that shader's; drawn bumped with the shader's own look (ngl_vk.h
    // metal_cube) and its literals: the rim at saturate(3.546 x (0.337 - N.V)) toward (0.294 0.316 0.412) (c254.y, z
    // and c255.xyz, rn20)
    MetalPass metal_pass;
    if (samplers.env && !d.lightmap && samplers.normal && samplers.env_mask && REXCVAR_GET(cod3_debug_ngl_side_metal) &&
        IsCubeTexture(base, samplers.env) && (metal_pass = ReadMetalPass(base, s.node)).ok) {
      constexpr float kMetalRimEdge = 0.337f;   // -c254.y
      constexpr float kMetalRimScale = 3.546f;  // -c254.z
      constexpr float kMetalRim[3] = {0.294f, 0.316f, 0.412f};  // c255.xyz
      d.metal_cube = CubeFor(base, samplers.env);
      d.normal_map = TextureFor(base, samplers.normal);
      d.specular_map = TextureFor(base, samplers.env_mask);
      d.metal_scratch = samplers.scratch ? TextureFor(base, samplers.scratch) : 0;
      d.metal_brightness = metal_pass.brightness;
      d.metal_reflection = metal_pass.reflection;
      d.metal_power = metal_pass.power;
      std::memcpy(d.metal_hue, metal_pass.hue, sizeof d.metal_hue);
      std::memcpy(d.metal_rim, kMetalRim, sizeof d.metal_rim);
      d.metal_rim_edge = kMetalRimEdge;
      d.metal_rim_scale = kMetalRimScale;
      d.metal_cube_bias = metal_pass.cube_bias;
      if (metal_pass.sun_bound) {  // its c28, for the light below as rn31's (the way stays the light block's)
        fabric_read = true;
        fabric_pass_read.sun_bound = true;
        std::memcpy(fabric_pass_read.sun_color, metal_pass.sun_color, sizeof fabric_pass_read.sun_color);
      }
      if (!d.metal_cube || !d.normal_map || !d.specular_map) {  // drawn shiny, as before
        d.metal_cube = d.normal_map = d.specular_map = d.metal_scratch = 0;
        fabric_read = false;
        fabric_pass_read = FabricPass{};
      } else {
        left_out[d.metal_scratch ? "(of those, the guns' metal: CFC45995D1294CCE, with a scratch map)"
                                 : "(of those, the guns' metal: CFC45995D1294CCE)"]++;
      }
      if (REXCVAR_GET(cod3_debug_ngl_side)) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 3) {
          REXLOG_INFO("cod3: ngl side: rn34 gun metal (object {:08X}): cube {}, scratch {}, brightness {:.4g}, "
                      "reflection {:.4g}, power {:.4g}, hue {:.4g} {:.4g} {:.4g}, cube bias {:.4g}, sun {} {:.3f} "
                      "{:.3f} {:.3f}", object, d.metal_cube, d.metal_scratch, metal_pass.brightness,
                      metal_pass.reflection, metal_pass.power, metal_pass.hue[0], metal_pass.hue[1], metal_pass.hue[2],
                      metal_pass.cube_bias, metal_pass.sun_bound ? "bound" : "not bound", metal_pass.sun_color[0],
                      metal_pass.sun_color[1], metal_pass.sun_color[2]);
          LogPassBinding(base, s.node, "metal");
        }
      }
    }
    LensPass lens_pass;  // rn40 (ReadLensPass)
    // rn40: the glass (FC767C09DBD34C2A: an environment cube, no normal map); rn40e: the eyes (5931940F174EA055: a
    // Reflection_Cube_Map, a normal map the shader doesn't bend the normal with, no specular map)
    const bool glass_like = samplers.env && !samplers.normal && REXCVAR_GET(cod3_debug_ngl_side_lens);
    const bool eyes_like = samplers.reflection_cube && !samplers.env && !samplers.env_mask &&
                           REXCVAR_GET(cod3_debug_ngl_side_eyes);
    const uint32_t lens_map = d.lightmap || !samplers.color ? 0
                              : glass_like                  ? samplers.env
                              : eyes_like                   ? samplers.reflection_cube
                                                            : 0;
    if (d.metal_cube) {
      // rn34: drawn as the guns' metal (above)
    } else if (lens_map && IsCubeTexture(base, lens_map) &&
               (lens_pass = ReadLensPass(base, s.node, glass_like ? kLensGlass : kLensEyes)).ok) {
      // [rexglue-native] 2026-11-10 rn40: glass with an environment cube (the goggle lenses), as FC767C09DBD34C2A draws
      // it (ngl_vk.h lens_cube), blended by its alpha below, where the draw's alpha is chosen; rn40e: or the eyes, as
      // 5931940F174EA055 draws them (opaque, the sun shadowed as the treads')
      const LensRegisters& registers = *lens_pass.registers;
      d.lens_cube = CubeFor(base, lens_map);
      d.lens_opaque = registers.opaque ? 1 : 0;
      d.lens_reflection = lens_pass.reflection;
      d.lens_power = registers.power;
      d.lens_cube_bias = lens_pass.bias;
      std::memcpy(d.lens_sun_place, lens_pass.sun_place, sizeof d.lens_sun_place);
      std::memcpy(d.lens_sun_color, lens_pass.sun_color, sizeof d.lens_sun_color);
      std::memcpy(d.lens_tint, registers.tint, sizeof d.lens_tint);
      d.lens_shadow_floor = lens_pass.floor;
      d.lens_shadow_fade = lens_pass.fade;
      // rn40m: a check of the eyes' parts (cod3_debug_ngl_side_eyes_show)
      d.lens_show = registers.opaque
                        ? static_cast<uint32_t>(std::clamp(REXCVAR_GET(cod3_debug_ngl_side_eyes_show), 0, 3))
                        : 0u;
      left_out[!d.lens_cube          ? "(of those, a cube that can't be made: drawn lit, opaque)"
               : registers.opaque ? "(of those, the eyes with a cube: 5931940F174EA055)"
                                  : "(of those, glass with a cube: FC767C09DBD34C2A, blended)"]++;
      if (REXCVAR_GET(cod3_debug_ngl_side)) {
        static std::atomic<int> said[2];  // (zero as statics)
        if (said[registers.opaque ? 1 : 0].fetch_add(1, std::memory_order_relaxed) < (registers.opaque ? 12 : 4)) {
          // rn40h: and the light kept for its object (its age in frames; none when the game's draws never kept one)
          std::string kept = "none";
          {
            std::lock_guard<std::mutex> lock(g_model_light_mutex);
            const auto it = g_model_lights.find(object);
            if (it != g_model_lights.end()) {
              kept = std::to_string(static_cast<int64_t>(frame) - static_cast<int64_t>(it->second.frame)) +
                     " frames old, row xx red " +
                     std::to_string(it->second.rows[0][0]);
            }
          }
          REXLOG_INFO("cod3: ngl side: rn40h eyes object {:08X} (frame {}): kept light {}", object, frame, kept);
          float at[3] = {};
          for (int i = 0; i < 3 && Readable(object + 0x38); ++i) at[i] = g::ReadF32(base, object + 0x30 + 4 * i);
          // rn40f: and the color map it is drawn with (and the set of looks it was picked from, when it was)
          const auto own_name = [&](uint32_t texture) -> std::string {
            const uint32_t name = texture && Readable(texture + kTextureName) ? g::ReadU32(base, texture + kTextureName)
                                                                                : 0;
            return Pointer(name) && Readable(name) ? InlineName(base, name + 4) : std::string("-");
          };
          REXLOG_INFO("cod3: ngl side: rn40 {} (object {:08X}): cube {}, reflection {:.4g}, bias {:.4g}, sun at "
                      "{:.1f} {:.1f} {:.1f} (the object at {:.1f} {:.1f} {:.1f}), color {:.3f} {:.3f} {:.3f}, "
                      "shadow floor {:.3g}, fade {:.3g}; color map {:08X} \"{}\" (looks {:08X} \"{}\", frame {})",
                      registers.opaque ? "eyes" : "glass", object, d.lens_cube, lens_pass.reflection,
                      lens_pass.bias, lens_pass.sun_place[0], lens_pass.sun_place[1], lens_pass.sun_place[2], at[0],
                      at[1], at[2], lens_pass.sun_color[0], lens_pass.sun_color[1], lens_pass.sun_color[2],
                      lens_pass.floor, lens_pass.fade, samplers.color, own_name(samplers.color), samplers.looks,
                      samplers.looks ? own_name(samplers.looks) : std::string("-"), look_frame.value);
        }
      }
    } else if (samplers.env && !d.lightmap) {  // side88: shiny (the guns): the environment map times the specular
                                                // map, added
      d.env_texture = TextureFor(base, samplers.env);
      d.env_mask = samplers.env_mask ? TextureFor(base, samplers.env_mask) : 0;
      left_out[d.env_mask ? "(of those, shiny: environment map x specular map)" : "(of those, shiny: environment map)"]++;
      // rn40: glass with a cube that wasn't taken as FC767C09DBD34C2A's, with cod3_debug_ngl_side, to see why
      if (REXCVAR_GET(cod3_debug_ngl_side) && lens_pass.why[0]) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 4) {
          REXLOG_INFO("cod3: ngl side: rn40 a cube without a normal map (object {:08X}) not taken as glass: {}",
                      object, lens_pass.why);
          LogPassBinding(base, s.node, "not taken as glass");
        }
      }
      // rn34: a gun whose pass isn't the metal shader's, once with cod3_debug_ngl_side, to see what binds it
      if (REXCVAR_GET(cod3_debug_ngl_side) && samplers.normal && IsCubeTexture(base, samplers.env)) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 3) {
          REXLOG_INFO("cod3: ngl side: rn34 shiny with a cube (object {:08X}), not taken as the metal shader's: "
                      "specular map {:08X}, scratch {:08X}", object, samplers.env_mask, samplers.scratch);
          LogPassBinding(base, s.node, "shiny with a cube");
        }
      }
    } else if (samplers.normal && !d.lightmap && REXCVAR_GET(cod3_debug_ngl_side_bumped)) {  // side117: per-pixel
                                                                                            // lit (CDBAC602): bumped
      // rn40e: an eyes cube that wasn't taken as 5931940F174EA055's, with cod3_debug_ngl_side, to see why
      if (REXCVAR_GET(cod3_debug_ngl_side) && eyes_like && lens_pass.why[0]) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 4) {
          REXLOG_INFO("cod3: ngl side: rn40e a reflection cube (object {:08X}) not taken as the eyes: {}", object,
                      lens_pass.why);
          LogPassBinding(base, s.node, "not taken as the eyes");
        }
      }
      d.normal_map = TextureFor(base, samplers.normal);
      d.specular_map = samplers.env_mask ? TextureFor(base, samplers.env_mask) : 0;
      left_out[d.specular_map ? "(of those, bumped: normal map, specular map)" : "(of those, bumped: normal map)"]++;
      // rn40x: the helmets' two shaders by their materials' samplers: 1D4725CE's hold neither swatch, ADC02DCE's both
      const bool helmet_samplers = (!samplers.normal_swatch && !samplers.noise_swatch) ||
                                   (samplers.normal_swatch && samplers.noise_swatch);
      // [rexglue-native] 2026-11-09 rn24: the uniforms (the fabric pixel shader 3BA30872B3955C23), with the
      // shader's own literals: the swatch at (uv - 0.5) x 28.717 + 0.5 (c252.y), the diffuse x 0.51 (c254.w) lifted
      // toward pow(diffuse, (0.50694, 0.583088, 0.71)) (c255.x, w, z), highlights of power 30 (c254.z)
      if (d.specular_map && samplers.fabric_mask && samplers.normal_swatch && REXCVAR_GET(cod3_debug_ngl_side_fabric)) {
        constexpr float kFabricSwatchRepeat = 28.717f;
        constexpr float kFabricDiffuseScale = 0.51f;
        constexpr float kFabricSheen[3] = {0.50694f, 0.583088f, 0.71f};
        constexpr float kFabricSpecularPower = 30.0f;
        // rn30: the soldiers' own uniforms are drawn by 21168F53BCF9D156 instead (rn28; the pass's binding tells,
        // ReadFabricPass), its literals read from the register file at its draws (rn29: c253 (28.717 3 2 0), c254
        // (0.655 50 -1 -0.5), c255 (0.5 -1 0 1)): the same swatch and edge, no 0.51, one sheen power 0.655 for all
        // three channels (c254.x), highlights of power 50 (c254.y), and the sun never under COD_SHADOW_GREYLEVEL,
        // eased toward 1 by COD_SHADOW_FARZ x the view depth (their vertex shader 85DA0D7EC65AC77C's o1.w). Some
        // bodies (bangboom's, papajack's) are also drawn by ADC02DCE4FDC9253, bound the same way; taken as these.
        constexpr float kSoldierFabricSheen = 0.655f;
        constexpr float kSoldierFabricSpecularPower = 50.0f;
        fabric_pass_read = ReadFabricPass(base, s.node);
        fabric_read = true;
        const FabricPass& fabric_pass = fabric_pass_read;
        d.fabric_mask = TextureFor(base, samplers.fabric_mask);
        d.normal_swatch = TextureFor(base, samplers.normal_swatch);
        d.fabric_swatch_repeat = kFabricSwatchRepeat;
        d.fabric_diffuse_scale = fabric_pass.third_person ? 1.0f : kFabricDiffuseScale;
        std::memcpy(d.fabric_sheen, kFabricSheen, sizeof d.fabric_sheen);
        if (fabric_pass.third_person) {
          for (float& power : d.fabric_sheen) power = kSoldierFabricSheen;
        }
        d.fabric_specular_power = fabric_pass.third_person ? kSoldierFabricSpecularPower : kFabricSpecularPower;
        d.fabric_shadow_floor = fabric_pass.shadow_floor;
        d.fabric_shadow_fade = fabric_pass.shadow_fade;
        if (d.fabric_mask && d.normal_swatch) {
          left_out[fabric_pass.third_person ? "(of those, the soldiers' own fabric: 21168F53BCF9D156)"
                                            : "(of those, the uniforms' fabric: mask and woven swatch)"]++;
        }
        if (fabric_pass.third_person && REXCVAR_GET(cod3_debug_ngl_side)) {  // rn30: the values as read, a few times
          static std::atomic<int> said{0};
          if (said.fetch_add(1, std::memory_order_relaxed) < 3) {
            REXLOG_INFO("cod3: ngl side: rn30 soldier fabric (object {:08X}): shadow floor {:.4g}, fade {:.4g}", object,
                        fabric_pass.shadow_floor, fabric_pass.shadow_fade);
          }
        }
        if (REXCVAR_GET(cod3_debug_ngl_side)) LogPassBinding(base, s.node, "fabric");  // rn25
      } else if (d.specular_map && !samplers.fabric_mask && !samplers.detail &&
                 helmet_samplers && REXCVAR_GET(cod3_debug_ngl_side_helmets) &&
                 (helmet_pass = ReadHelmetPass(base, s.node)).ok) {
        // [rexglue-native] 2026-11-10 rn40t: the soldiers' helmets (ReadHelmetPass), drawn as the skin with
        // 1D4725CEF0551EBB's numbers. That shader has no noise swatch: its specular map stands in the swatch's place
        // with a share of 0, so the highlights' mask is 2.38486 x the specular map alone. Its sun (color and place) is
        // taken as the uniforms' below.
        // rn40u: told by the pass, before the skin's test (a material with a Noise_Swatch was taken by the skin's
        // branch, found not to be the skin's pass, rows from c19, and left as a generic bumped draw). 21168F53 (the
        // uniforms) binds the same registers; its materials have a Fabric_Mask and are taken above.
        // rn40x: and by the material's samplers: with neither swatch 1D4725CE, with both (and no Fabric_Mask)
        // ADC02DCE4FDC9253, drawn the same way with its woven swatch folded in and its noise swatch in the
        // highlights' mask (kWoven...); with one only, neither (as before).
        const bool woven = samplers.normal_swatch && samplers.noise_swatch;
        d.fabric_diffuse_scale = 1.0f;
        d.fabric_specular_power = kHelmetSpecularPower;
        d.skin_edge_lift = kHelmetEdgeLift;
        if (woven) {
          d.noise_swatch = TextureFor(base, samplers.noise_swatch);
          d.fabric_swatch_repeat = kWovenNoiseRepeat;
          d.skin_specular_scale = kWovenSpecularScale;
          d.skin_noise_mix = kWovenNoiseMix;
          d.skin_swatch = TextureFor(base, samplers.normal_swatch);
          d.skin_swatch_repeat = kWovenSwatchRepeat;
        } else {
          d.noise_swatch = d.specular_map;
          d.fabric_swatch_repeat = 1.0f;
          d.skin_specular_scale = kHelmetSpecularScale;
          d.skin_noise_mix = 0.0f;
        }
        d.fabric_shadow_floor = helmet_pass.shadow_floor;
        d.fabric_shadow_fade = helmet_pass.shadow_fade;
        fabric_read = true;
        helmet_read = true;
        fabric_pass_read = FabricPass{};
        fabric_pass_read.sun_bound = true;
        std::memcpy(fabric_pass_read.sun_color, helmet_pass.sun_color, sizeof fabric_pass_read.sun_color);
        fabric_pass_read.sun_placed = true;
        std::memcpy(fabric_pass_read.sun_place, helmet_pass.sun_place, sizeof fabric_pass_read.sun_place);
        left_out[woven ? "(of those, woven helmets and bodies: ADC02DCE4FDC9253)"
                       : "(of those, the soldiers' helmets: 1D4725CEF0551EBB)"]++;
        if (REXCVAR_GET(cod3_debug_ngl_side)) {  // once for each material, up to 12: what is taken as a helmet
          static std::mutex mutex;
          static std::vector<uint32_t> logged;
          bool fresh = false;
          {
            std::lock_guard<std::mutex> lock(mutex);
            if (logged.size() < 12 && std::find(logged.begin(), logged.end(), material) == logged.end()) {
              logged.push_back(material);
              fresh = true;
            }
          }
          if (fresh) {
            const uint32_t name_at = IsTexture(base, samplers.color) ? g::ReadU32(base, samplers.color + kTextureName)
                                                                     : 0;
            const std::string name = Pointer(name_at) && Readable(name_at) ? InlineName(base, name_at + 4)
                                                                           : std::string("-");
            REXLOG_INFO("cod3: ngl side: rn40t helmet {} (material {:08X}, object {:08X}, color map {}): sun {:.3f} "
                        "{:.3f} {:.3f} from {:.0f} {:.0f} {:.0f}, floor {:.4g}, fade {:.4g}; rn40w samplers: {}; rn40x "
                        "noise {}, woven swatch {}", woven ? "ADC02DCE4FDC9253" : "1D4725CEF0551EBB", material, object,
                        name, helmet_pass.sun_color[0], helmet_pass.sun_color[1], helmet_pass.sun_color[2],
                        helmet_pass.sun_place[0], helmet_pass.sun_place[1], helmet_pass.sun_place[2],
                        helmet_pass.shadow_floor, helmet_pass.shadow_fade, SamplerNames(base, material),
                        d.noise_swatch, d.skin_swatch);
          }
        }
      } else if (d.specular_map && samplers.noise_swatch && !samplers.fabric_mask &&
                 REXCVAR_GET(cod3_debug_ngl_side_skin)) {
        // rn32: the soldiers' heads, when the pass is the game's skin shader 3949D29E09A78AA1 (its rows from c20;
        // the arms' CDBAC6026B525050 has a Noise_Swatch too, rows from c18, and stays as it was). Its literals at its
        // draws (rn29): c253 (1.215 0.5 0.38 68.528), c254 (0 1 0.4 68.528), c255 (11.018 0.5 -0.5 -1).
        constexpr float kSkinNoiseRepeat = 11.018f;     // c255.x
        constexpr float kSkinSpecularPower = 68.528f;   // c253.w
        constexpr float kSkinEdgeLift = 1.215f;         // c253.x
        constexpr float kSkinSpecularScale = 0.4f;      // c254.z
        constexpr float kSkinNoiseMix = 0.38f;          // c253.z
        fabric_pass_read = ReadFabricPass(base, s.node);
        if (fabric_pass_read.skin) {
          fabric_read = true;
          d.noise_swatch = TextureFor(base, samplers.noise_swatch);
          d.fabric_swatch_repeat = kSkinNoiseRepeat;
          d.fabric_diffuse_scale = 1.0f;
          d.fabric_specular_power = kSkinSpecularPower;
          d.skin_edge_lift = kSkinEdgeLift;
          d.skin_specular_scale = kSkinSpecularScale;
          d.skin_noise_mix = kSkinNoiseMix;
          d.fabric_shadow_floor = fabric_pass_read.shadow_floor;
          d.fabric_shadow_fade = fabric_pass_read.shadow_fade;
          if (d.noise_swatch) left_out["(of those, the soldiers' skin: 3949D29E09A78AA1)"]++;
          if (REXCVAR_GET(cod3_debug_ngl_side)) {
            static std::atomic<int> said{0};
            if (said.fetch_add(1, std::memory_order_relaxed) < 3) {
              REXLOG_INFO("cod3: ngl side: rn32 soldier skin (object {:08X}): shadow floor {:.4g}, fade {:.4g}, sun {} "
                          "{:.3f} {:.3f} {:.3f} from {:.1f} {:.1f} {:.1f}", object, fabric_pass_read.shadow_floor,
                          fabric_pass_read.shadow_fade, fabric_pass_read.sun_bound ? "bound" : "not bound",
                          fabric_pass_read.sun_color[0], fabric_pass_read.sun_color[1], fabric_pass_read.sun_color[2],
                          fabric_pass_read.sun_place[0], fabric_pass_read.sun_place[1], fabric_pass_read.sun_place[2]);
            }
            LogPassBinding(base, s.node, "skin");
          }
        }
      } else if (d.specular_map && samplers.detail && !samplers.fabric_mask && !samplers.noise_swatch &&
                 REXCVAR_GET(cod3_debug_ngl_side_detail)) {
        // [rexglue-native] 2026-11-09 rn36: the vehicles' detail-mapped metal (ReadDetailPass), when the pass is bound
        // as C90DF51FF5AAF4F8's or 8E7F713B9E3C1FAD's; its sun (color and place) taken as the uniforms' below
        const DetailPass detail_pass = ReadDetailPass(base, s.node);
        if (detail_pass.ok && (d.detail_map = TextureFor(base, samplers.detail)) != 0) {
          d.detail_repeat = detail_pass.repeat;
          d.detail_scale = detail_pass.detail_scale;
          d.detail_normal_scale = detail_pass.normal_scale;
          d.detail_specular_power = detail_pass.power;
          d.fabric_shadow_floor = detail_pass.shadow_floor;
          d.fabric_shadow_fade = detail_pass.shadow_fade;
          fabric_read = true;
          detail_read = true;
          fabric_pass_read = FabricPass{};
          fabric_pass_read.sun_bound = true;
          std::memcpy(fabric_pass_read.sun_color, detail_pass.sun_color, sizeof fabric_pass_read.sun_color);
          fabric_pass_read.sun_placed = true;
          std::memcpy(fabric_pass_read.sun_place, detail_pass.sun_place, sizeof fabric_pass_read.sun_place);
          left_out[detail_pass.shadowed ? "(of those, the vehicles' detail-mapped metal: C90DF51FF5AAF4F8)"
                                        : "(of those, the vehicles' detail-mapped metal: 8E7F713B9E3C1FAD)"]++;
          if (REXCVAR_GET(cod3_debug_ngl_side)) {
            static std::atomic<int> said{0};
            if (said.fetch_add(1, std::memory_order_relaxed) < 4) {
              REXLOG_INFO("cod3: ngl side: rn36 detail-mapped metal (object {:08X}, {}): repeat {:.4g}, z scales "
                          "{:.4g} {:.4g}, power {:.4g}, sun {:.3f} {:.3f} {:.3f} from {:.0f} {:.0f} {:.0f}, floor "
                          "{:.4g}, fade {:.4g}", object, detail_pass.shadowed ? "in the shadow range" : "beyond it",
                          detail_pass.repeat, detail_pass.normal_scale, detail_pass.detail_scale, detail_pass.power,
                          detail_pass.sun_color[0], detail_pass.sun_color[1], detail_pass.sun_color[2],
                          detail_pass.sun_place[0], detail_pass.sun_place[1], detail_pass.sun_place[2],
                          detail_pass.shadow_floor, detail_pass.shadow_fade);
            }
          }
        } else {
          d.detail_map = 0;
        }
      } else if (d.specular_map && !samplers.detail && !samplers.fabric_mask && !samplers.noise_swatch &&
                 REXCVAR_GET(cod3_debug_ngl_side_vehicle)) {
        // [rexglue-native] 2026-11-10 rn39: the vehicles' bumped parts (ReadVehiclePass), when the pass is bound as
        // one of their five shaders' (VehicleLayout); its sun (color and place) taken as the uniforms' below
        const VehiclePass vehicle_pass = ReadVehiclePass(base, s.node, object);
        if (vehicle_pass.ok) {
          d.vehicle_bumped = 1;
          d.vehicle_normal_scale = vehicle_pass.normal_scale;
          d.vehicle_specular_power = vehicle_pass.power;
          d.fabric_shadow_floor = vehicle_pass.shadow_floor;
          d.fabric_shadow_fade = vehicle_pass.shadow_fade;
          fabric_read = true;
          vehicle_read = true;
          fabric_pass_read = FabricPass{};
          fabric_pass_read.sun_bound = true;
          std::memcpy(fabric_pass_read.sun_color, vehicle_pass.sun_color, sizeof fabric_pass_read.sun_color);
          fabric_pass_read.sun_placed = true;
          std::memcpy(fabric_pass_read.sun_place, vehicle_pass.sun_place, sizeof fabric_pass_read.sun_place);
          if (left_out.on()) {
            left_out[std::string("(of those, the vehicles' bumped parts: ") + vehicle_pass.shader + ")"]++;
          }
          if (REXCVAR_GET(cod3_debug_ngl_side)) {
            static std::atomic<int> said{0};
            if (said.fetch_add(1, std::memory_order_relaxed) < 8) {
              REXLOG_INFO("cod3: ngl side: rn39 bumped vehicle part (object {:08X}, {}): normal scale {:.4g}, power "
                          "{:.4g}, sun {:.3f} {:.3f} {:.3f} from {:.0f} {:.0f} {:.0f}, floor {:.4g}, fade {:.4g}", object,
                          vehicle_pass.shader, vehicle_pass.normal_scale, vehicle_pass.power, vehicle_pass.sun_color[0],
                          vehicle_pass.sun_color[1], vehicle_pass.sun_color[2], vehicle_pass.sun_place[0],
                          vehicle_pass.sun_place[1], vehicle_pass.sun_place[2], vehicle_pass.shadow_floor,
                          vehicle_pass.shadow_fade);
            }
          }
        } else if (REXCVAR_GET(cod3_debug_ngl_side) && frame >= kWatchFrom && IsTexture(base, samplers.color)) {
          // rn39c: why a watched color map's draw isn't taken (rn39b: some of The Mace's tank objects were, others
          // with the same color maps weren't), once per object, up to 16 lines
          static std::mutex mutex;
          static std::vector<uint32_t> seen;
          const uint32_t name_at = g::ReadU32(base, samplers.color + kTextureName);
          const std::string name = Pointer(name_at) && Readable(name_at) ? InlineName(base, name_at + 4) : "";
          const std::string watch = Lower(REXCVAR_GET(cod3_debug_ngl_side_watch));
          bool say = false;
          if (!watch.empty() && Lower(name).find(watch) != std::string::npos) {
            std::lock_guard<std::mutex> lock(mutex);
            if (seen.size() < 16 && std::find(seen.begin(), seen.end(), object) == seen.end()) {
              seen.push_back(object);
              say = true;
            }
          }
          if (say) {
            REXLOG_INFO("cod3: ngl side: rn39c not taken as a vehicle part (frame {}): object {:08X}, \"{}\", node "
                        "{:08X}, rows from c{}, as {}: {}; normal scale {:.4g}, power {:.4g}, sun {:.3f} {:.3f} {:.3f} "
                        "from {:.0f} {:.0f} {:.0f} ({:.0f} off), floor {:.4g}, fade {:.4g}",
                        frame, object, name, s.node,
                        vehicle_pass.rows_at, *vehicle_pass.shader ? vehicle_pass.shader : "-", vehicle_pass.why,
                        vehicle_pass.normal_scale, vehicle_pass.power, vehicle_pass.sun_color[0],
                        vehicle_pass.sun_color[1], vehicle_pass.sun_color[2], vehicle_pass.sun_place[0],
                        vehicle_pass.sun_place[1], vehicle_pass.sun_place[2], vehicle_pass.away,
                        vehicle_pass.shadow_floor, vehicle_pass.shadow_fade);
            LogPassBinding(base, s.node, ("rn39c " + name).c_str());
          }
        }
      } else if (!d.specular_map && !samplers.fabric_mask && !samplers.detail && !samplers.normal_swatch &&
                 !samplers.noise_swatch && REXCVAR_GET(cod3_debug_ngl_side_plain_heads) &&
                 (plain_head_pass = ReadPlainHeadPass(base, s.node)).ok) {
        // [rexglue-native] 2026-11-10 rn40y: the heads drawn without a specular map (ReadPlainHeadPass), as the skin
        // with every extra off: the normal map stands in for the specular map and the noise swatch, with a highlights'
        // scale of 0 (no highlights) and an edge lift of 1; the sun's color, floor and fade from the pass, its way the
        // light's own (the shader's comes from its vertex shader); the soft terminator (ngl_vk.h skin_terminator)
        d.specular_map = d.normal_map;
        d.noise_swatch = d.normal_map;
        d.fabric_swatch_repeat = 1.0f;
        d.fabric_diffuse_scale = 1.0f;
        d.fabric_specular_power = 1.0f;
        d.skin_edge_lift = 1.0f;
        d.skin_specular_scale = 0.0f;
        d.skin_noise_mix = 0.0f;
        d.skin_terminator_bias = kPlainHeadTerminatorBias;
        d.skin_terminator_scale = kPlainHeadTerminatorScale;
        d.fabric_shadow_floor = plain_head_pass.shadow_floor;
        d.fabric_shadow_fade = plain_head_pass.shadow_fade;
        fabric_read = true;
        fabric_pass_read = FabricPass{};
        fabric_pass_read.sun_bound = true;
        std::memcpy(fabric_pass_read.sun_color, plain_head_pass.sun_color, sizeof fabric_pass_read.sun_color);
        left_out["(of those, the heads without a specular map: 2340DF59702A9AD4)"]++;
        if (REXCVAR_GET(cod3_debug_ngl_side)) {
          static std::atomic<int> said{0};
          if (said.fetch_add(1, std::memory_order_relaxed) < 4) {
            REXLOG_INFO("cod3: ngl side: rn40y plain head (material {:08X}, object {:08X}): sun {:.3f} {:.3f} {:.3f}, "
                        "floor {:.4g}, fade {:.4g}; samplers: {}", material, object, plain_head_pass.sun_color[0],
                        plain_head_pass.sun_color[1], plain_head_pass.sun_color[2], plain_head_pass.shadow_floor,
                        plain_head_pass.shadow_fade, SamplerNames(base, material));
          }
        }
      }
    }
    if (!s.old_class) {
      SurveyMaterial(base, material, object, skinned,
                     d.env_texture ? "shiny" : d.normal_map ? "bumped" : d.lightmap ? "lightmapped" : "plain", frame);
    }
    const char* sun_source = "none";  // rn42y: where the sun handed to ngl_vk came from, for LogLiveLight (debug only)
    {
      Sun sun;
      if (!s.old_class && StateSun(base, g::ReadU32(base, s.node + kNodeShaderState), sun)) {
        static bool said = false;
        if (!said) {
          said = true;
          REXLOG_INFO("cod3: ngl side: the sun travels {:.3f} {:.3f} {:.3f}, color {:.3f} {:.3f} {:.3f}, ambient "
                      "{:.3f} {:.3f} {:.3f}", sun.direction[0], sun.direction[1], sun.direction[2], sun.color[0],
                      sun.color[1], sun.color[2], sun.ambient[0], sun.ambient[1], sun.ambient[2]);
        }
        g_sun = sun;
      }
      if (world_lit) {
        WorldLight(base, d, sun.ok ? sun : g_sun, samplers.lightmap, samplers.color);
        if (!g_quiet) SurveyWorldLights(base, object);  // side182: on the frames saved
        if (d.lightmap_scale > 0.0f && REXCVAR_GET(cod3_debug_ngl_side_world_nearby_lights)) {
          NearbyLights(base, object, d);  // side183
          if (d.point_light_count) left_out["(of those, lit by their nearby lights too)"]++;
        }
        if (d.lightmap_scale > 0.0f) left_out["(of those, the lightmapped world lit as the game's shaders do)"]++;
      }
      if (d.shading == NGL_VK_SHADE_LIGHT) {
        Sun own;
        const bool has_own = ObjectSun(base, object, own);
        const int model_light = REXCVAR_GET(cod3_debug_ngl_side_model_light);
        const Sun& scene_sun = sun.ok ? sun : g_sun;
        if (has_own && model_light != 0 && scene_sun.ok) {  // side168: the scene's sun with the set's own lights
          Sun both = own;
          std::memcpy(both.direction, scene_sun.direction, sizeof both.direction);
          std::memcpy(both.color, scene_sun.color, sizeof both.color);
          if (model_light == 1) {  // the set's first light and its others after the sun
            both.more = std::min<uint32_t>(own.more + 1, 3);
            std::memcpy(both.more_direction[0], own.direction, sizeof own.direction);
            std::memcpy(both.more_color[0], own.color, sizeof own.color);
            for (uint32_t k = 1; k < both.more; ++k) {
              std::memcpy(both.more_direction[k], own.more_direction[k - 1], sizeof own.more_direction[k - 1]);
              std::memcpy(both.more_color[k], own.more_color[k - 1], sizeof own.more_color[k - 1]);
            }
          }
          LightWith(d, both);
          d.light_saturate = 0;  // the per-pixel shaders don't hold the light to 1
          left_out["(of those, lit by the scene's sun and their own light set)"]++;
          sun_source = sun.ok ? "its node's" : "the last node's";  // rn42y
        } else if (has_own) {  // side89: its own light set
          LightWith(d, own);
          left_out["(of those, lit by their own light set)"]++;
          sun_source = "its light set's";  // rn42y
        } else {
          LightWith(d, sun.ok ? sun : g_sun);
          if (d.lit) left_out["(of those, lit by the game's sun)"]++;
          sun_source = sun.ok ? "its node's" : "the last node's";  // rn42y
        }
#if !defined(COD3_MP)
        if (camera->second.front) {  // side116: the front scene (the gun, the arms) as its own draw was lit
          FrontLight front;
          bool have = false;
          {
            std::lock_guard<std::mutex> lock(g_front_mutex);
            const auto it = g_front_lights.find(object);
            if (it != g_front_lights.end()) {
              front = it->second;
              have = true;
            }
          }
          if (have) {
            Sun lit;
            lit.ok = true;
            std::memcpy(lit.ambient, front.ambient, sizeof lit.ambient);
            const float length = std::sqrt(front.way[0] * front.way[0] + front.way[1] * front.way[1] +
                                           front.way[2] * front.way[2]);
            for (int i = 0; i < 3; ++i) {  // no way set (the gun's): the main light reaches nothing, as N.0 = 0
              // the way points to the light (the shadow light's is the sun's place x 10^6); ngl_vk takes where
              // the light travels
              lit.direction[i] = length > 1e-4f ? -front.way[i] / length : 0.0f;
              lit.color[i] = length > 1e-4f ? front.color[i] : 0.0f;
            }
            if (!(length > 1e-4f)) lit.direction[2] = -1.0f;
            LightWith(d, lit);
            left_out["(of those, the front scene's light from its own draw)"]++;
            sun_source = "the front scene's own draw";  // rn42y
          }
        }
#endif
        // side106: what light a soldier gets. The game's light grid (0x824224F0, called by 0x82427920 for each
        // soldier) weights the sun by how much of it reaches where the soldier stands, so a soldier's set should be
        // darker under the trees; this shows whether the native side sees that.
        static int skinned_shown = 0, front_shown = 0;  // side109: the front scene's (the gun, the arms) too
        static int rigid_shown = 0;  // side167: the rigid ones too (Black Burn's tanks, Saint-Lo's props)
        // side124: the counter that let the line through counts it (a front skinned surface, the arms or
        // cod3mp's side-selection soldier, passed as skinned but counted as front, so it was logged every frame)
        const bool front_scene = camera->second.front;
        int& shown = front_scene ? front_shown : skinned ? skinned_shown : rigid_shown;
        if (shown < (front_scene ? 16 : skinned ? 48 : 32)) {
          ++shown;
          const Sun& used = has_own ? own : (sun.ok ? sun : g_sun);
          REXLOG_INFO("cod3: ngl side: {} surface {:08X} of object {:08X} ({}, normal map {:08X}): {} light, ambient "
                      "{:.3f} {:.3f} {:.3f}, light color {:.3f} {:.3f} {:.3f} traveling {:.3f} {:.3f} {:.3f}, {} more",
                      front_scene ? (skinned ? "front skinned" : "front") : skinned ? "skinned" : "rigid", surface,
                      object, d.env_texture ? "shiny" : d.normal_map ? "bumped" : "plain", samplers.normal,
                      has_own ? "its own" : "the game's", used.ambient[0], used.ambient[1],
                      used.ambient[2], used.color[0], used.color[1], used.color[2], used.direction[0],
                      used.direction[1], used.direction[2], used.more);
        }
        // rn31: a uniform's sun as its pass binds it (ReadFabricPass): PROJCOLOR its color, and for the soldiers' own
        // (21168F53) the way from position_Light0 to the soldier, as that shader's normalize(c28 - position), taken at
        // the soldier's place (the light is some 2000 units off, so across a soldier it turns by about a degree)
        if (fabric_read && d.lit && fabric_pass_read.sun_bound) {
          const float* color = fabric_pass_read.sun_color;
          if (color[0] >= 0.0f && color[1] >= 0.0f && color[2] >= 0.0f && color[0] <= kSunColorMost &&
              color[1] <= kSunColorMost && color[2] <= kSunColorMost) {  // as StateSun (rn18: not the 0xCD fill)
            std::memcpy(d.light_color, color, sizeof d.light_color);
            left_out["(of those, the sun as their pass binds it)"]++;
            // rn42y; rn43a: whether its pass's values were those the game drew it with (DrawnValue)
            sun_source = t_drawn.used && !t_drawn.late ? "its pass binding (as drawn)"
                         : t_drawn.used               ? "its pass binding (partly as drawn)"
                                                      : "its pass binding (read now)";
          }
          // rn32: the skin too (3949D29E's c33); rn36: and the detail-mapped metal (C90DF51F's c27, 8E7F713B's c31)
          if ((fabric_pass_read.third_person || fabric_pass_read.skin || detail_read || vehicle_read || helmet_read) &&
              fabric_pass_read.sun_placed &&
              Readable(object + 0x38)) {
            float way[3];
            for (int i = 0; i < 3; ++i) {
              way[i] = g::ReadF32(base, object + 0x30 + 4 * i) - fabric_pass_read.sun_place[i];
            }
            const float length = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
            if (std::isfinite(length) && length > 1.0f) {
              for (int i = 0; i < 3; ++i) d.light_direction[i] = way[i] / length;
            }
          }
        }
      }
    }
    FogWith(base, s.scene, d);
    if (d.fog[1] > d.fog[0]) left_out["(of those, fogged)"]++;
    if (t_drawn.used || t_drawn.late) {  // rn43a: its pass's values (cod3_debug_ngl_side_drawn_values)
      left_out[!t_drawn.late  ? "(rn43a: pass values as the game drew them)"
               : t_drawn.used ? "(rn43a: pass values partly as the game drew them)"
                              : "(rn43a: pass values read now: none kept as drawn)"]++;  // (no commas in these)
      if (t_drawn.by_place) left_out["(rn43c: pass values kept by place)"]++;
    }
    // a soldier's DXT5 color map keeps something else in its alpha (side24 drew them whole without the test)
    d.alpha = !skinned && g_cut_out.count(d.texture) ? NGL_VK_ALPHA_TEST : NGL_VK_ALPHA_OPAQUE;
    // rn40: the game blends the glass by its alpha (blend 07060706); rn40e: not the helmets (00010001)
    if (d.lens_cube && !d.lens_opaque) d.alpha = NGL_VK_ALPHA_BLEND;
    left_out[d.texture ? (d.lightmap ? "(drawn with color map and lightmap)" : "(drawn with a color map)")
                       : "(drawn untextured)"]++;
    if (d.alpha == NGL_VK_ALPHA_TEST) left_out["(of those, alpha-tested)"]++;
    if (skinned) left_out["(of those, skinned)"]++;
    static int untextured_shown = 0;
    if (!d.texture && untextured_shown < 6) {
      ++untextured_shown;
      const uint32_t list = g::ReadU32(base, material + kMaterialSamplers);
      REXLOG_INFO("cod3: ngl side: untextured surface {:08X}: material {:08X} ({}), samplers {:08X}: {} / {}",
                  surface, material, Words(base, material, 8), list,
                  Readable(list) ? Words(base, list, 8) : std::string("-"),
                  Readable(list) ? Words(base, list + 0x20, 8) : std::string("-"));
      // side49: a soldier's uniform (material AD584300) names a Diffuse_Texture sampler whose texture (AB4920E8) isn't
      // an NGL texture, so it was drawn in its tint: what it points at, and one pointer deeper.
      for (uint32_t k = 0; Readable(list) && k < kMaxSamplers; ++k) {
        const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
        const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
        if (!Pointer(parameter)) break;
        if (IsTexture(base, texture) || !Readable(texture)) continue;
        REXLOG_INFO("cod3: ngl side: untextured surface {:08X} sampler {} <{}>: {:08X}: {} / {}", surface, k,
                    SamplerTextureName(base, parameter), texture, Words(base, texture, 8), Words(base, texture + 0x20, 8));
        for (uint32_t at = 0; at < 0x40; at += 4) {
          const uint32_t inner = g::ReadU32(base, texture + at);
          if (!Pointer(inner) || !Readable(inner) || inner == texture) continue;
          REXLOG_INFO("cod3: ngl side: untextured surface {:08X} sampler {} +{:02X} -> {:08X}{}: {}", surface, k, at,
                      inner, IsTexture(base, inner) ? " (an NGL texture)" : "", Words(base, inner, 8));
        }
      }
    }
    bool sane = true;
    for (uint32_t k = 0; k < 16; ++k) {
      d.world[k] = g::ReadF32(base, object + 4 * k);
      sane = sane && std::isfinite(d.world[k]);
    }
    if (!sane) {
      left_out["its world matrix isn't sane"]++;
      continue;
    }
    d.world[3] = d.world[7] = d.world[11] = 0.0f;
    d.world[15] = 1.0f;
    const char* light_source = "the light set's (not asked)";  // rn42t: for LogLiveLight (debug only)
    uint64_t light_kept = 0;
    float block_sun[3] = {-1.0f, -1.0f, -1.0f};  // rn42y: the light block's main light taken, -1 for none (debug)
    {  // side184: the light the game handed this model's own draws (cod3_debug_ngl_side_model_irradiance)
      const int irradiance = REXCVAR_GET(cod3_debug_ngl_side_model_irradiance);
      bool by_place = false, own_none = false;  // rn40j
      const bool wants_game_light = d.shading == NGL_VK_SHADE_LIGHT && d.lit && irradiance != 0 &&
                                    (irradiance == 1 || d.normal_map || d.env_texture);
      GameModelLight taken;  // rn42w
      const bool game_light =
          wants_game_light && GameLightFor(base, object, frame, d, &by_place, &own_none, &light_kept, &taken);
      if (wants_game_light) {
        light_source = game_light ? (by_place ? (own_none ? "by place (none of its own)" : "by place (its own too old)")
                                              : "its own")
                                  : (own_none ? "the light set's (none kept)" : "the light set's (its own too old)");
      }
      if (game_light && taken.main) std::memcpy(block_sun, taken.main_color, sizeof block_sun);  // rn42y
      // [rexglue-native] 2026-11-10 rn42w: the sun the game handed the model (cod3_debug_ngl_side_model_sun). Without
      // it a lit model's sun was its node's state sun, else g_sun (the last node's: rn42u, Saint-Lo's mesh 3 at 8224
      // 9926 took 0.960, 1.148, 0.960, 1.148 on frames in a row) or its light set's (The Mace's group at -900 -5200
      // went 0.773 to 0.373 within a burst): lit, dark, lit (owner 10:29). The game's own sun for the draw is its
      // light block's main light (0x82142360 fills it for each object; COD_SHADOW_PROJCOLOR = block +0x00, rn22), kept
      // with the rows. Not for the front scene (its own FrontLight).
      // rn42z: the draws whose pass binds their sun (fabric_read: the uniforms, skin, helmets, metal, vehicles) too,
      // with cod3_debug_ngl_side_model_sun_pass: the color only (their way stays the pass's position_Light0 when it
      // binds one). rn42z's The Mace: their pass-bound sun read 0.905 for most draws and 0.773, 0.764 or 0.373 for some
      // on some frames (the tank's group went 0.773, 0.773, 0.773, 0.905, 0.905, 0.905), while their light block's
      // main light was 0.905 on every frame (one object 0.875 against its pass's steady 0.905): the value block is
      // read after the game has written it again for other passes. A check only (off by default): at St-Bert the
      // block's main light of soldiers in shade is 0.509 to 0, where their shaders take the scene's 0.614.
      const bool pass_sun = fabric_read;
      if (game_light && taken.main && !camera->second.front && REXCVAR_GET(cod3_debug_ngl_side_model_sun) &&
          (!pass_sun || REXCVAR_GET(cod3_debug_ngl_side_model_sun_pass))) {
        const float length = std::sqrt(taken.main_way[0] * taken.main_way[0] + taken.main_way[1] * taken.main_way[1] +
                                       taken.main_way[2] * taken.main_way[2]);
        const bool keep_way = pass_sun && fabric_pass_read.sun_placed;
        for (int i = 0; i < 3; ++i) {
          d.light_color[i] = taken.main_color[i];
          // the way points to the light; ngl_vk takes its travel
          if (!keep_way) d.light_direction[i] = -taken.main_way[i] / length;
        }
        left_out[pass_sun ? "(of those, rn42z: the sun the game handed them in place of their pass's)"
                          : "(of those, rn42w: the sun the game handed them, its light block's main light)"]++;
        sun_source = pass_sun ? "its light block's main light (its pass binds one)"
                              : "its light block's main light";  // rn42y
      }
      if (wants_game_light && !game_light) {  // rn40j: why not (the light set's light is used)
        left_out[own_none ? "(lit models without the game's light: none kept for its object)"
                          : "(lit models without the game's light: its object's too old)"]++;
        // [rexglue-native] 2026-11-10 rn40z: which models those are (rn40y's census: about 40 a frame, drawn with the
        // light set's sun), once for each color map from kWatchFrom, up to 24 lines (debug only). rn40z3: and where
        // the model's origin falls in the picture (as rn35's: 0 to 1 across and down, -1 behind the eye), counted
        // each frame: whether the game left them out of its own view (culled) or drew them without our keeping it
        float across = -1.0f, down = -1.0f;
        if (REXCVAR_GET(cod3_debug_ngl_side)) {
          const float* vp = camera->second.view_projection;
          float clip[4] = {};
          for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 3; ++row) clip[column] += d.world[12 + row] * vp[4 * row + column];
            clip[column] += vp[12 + column];
          }
          across = clip[3] > 0.0f ? 0.5f + 0.5f * clip[0] / clip[3] : -1.0f;
          down = clip[3] > 0.0f ? 0.5f - 0.5f * clip[1] / clip[3] : -1.0f;
          const bool inside = across >= 0.0f && across <= 1.0f && down >= 0.0f && down <= 1.0f;
          left_out[inside ? "(lit models without the game's light: rn40z3 their origin in the picture)"
                          : "(lit models without the game's light: rn40z3 their origin outside it)"]++;
        }
        if (REXCVAR_GET(cod3_debug_ngl_side) && frame >= kWatchFrom) {
          static std::mutex mutex;
          static std::vector<uint32_t> seen;
          bool fresh = false;
          {
            std::lock_guard<std::mutex> lock(mutex);
            if (seen.size() < 24 && std::find(seen.begin(), seen.end(), samplers.color) == seen.end()) {
              seen.push_back(samplers.color);
              fresh = true;
            }
          }
          if (fresh) {
            const uint32_t name_at = IsTexture(base, samplers.color) ? g::ReadU32(base, samplers.color + kTextureName)
                                                                     : 0;
            const std::string name = Pointer(name_at) && Readable(name_at) ? InlineName(base, name_at + 4)
                                                                           : std::string("-");
            REXLOG_INFO("cod3: ngl side: rn40z lit model without the game's light ({}): color map {}, object {:08X} "
                        "at {:.0f} {:.0f} {:.0f} (in the picture at {:.3f} {:.3f}, frame {}), material {:08X}, {}, {}, "
                        "normal map {}, samplers: {}",
                        own_none ? "none kept for its object" : "its object's too old", name, object, d.world[12],
                        d.world[13], d.world[14], across, down, frame, material, skinned ? "skinned" : "rigid",
                        camera->second.front ? "front scene" : "world", d.normal_map, SamplerNames(base, material));
            // rn40z2: and its pass's binding: whether the game's shader for it reads the light block (slot 4) at all
            LogPassBinding(base, s.node, ("rn40z " + name).c_str());
          }
        }
      }
      if (game_light) {
        left_out["(of those, lit by the light the game handed them)"]++;
        if (by_place) {
          left_out[own_none ? "(of those, the light kept for a model where it stands: none kept for its object)"
                            : "(of those, the light kept for a model where it stands: its object's too old)"]++;
        }
        if (d.point_light_count) left_out["(of those, with its nearest point light)"]++;
        // [rexglue-native] 2026-11-09 rn25b: what the native fabric draws are lit by, from frame 2900, new values only
        // (rn25: many soldiers' GPU LIGHT_COLOR1 is 8-12, a fire; the native soldier at 3000 is far darker).
        // rn25c: with the kept rows as the game handed them (the world's xx yy zz, red, as the GPU's c18-c20) and how
        // many lit draws so far came in passes that didn't refill the block
        // rn27: one line per diffuse texture (where the GPU reads it, as the GPU side's fabric lines print tf5) and
        // per 300 frames, with the look set and frame it came from, to pair each soldier with the GPU's draw of it
        if (d.fabric_mask && REXCVAR_GET(cod3_debug_ngl_side) && frame >= 2900) {
          static std::vector<int64_t> seen;
          uint32_t diffuse_at = 0;
          {
            uint32_t fetch[6];
            if (samplers.color && ReadFetch(base, g::ReadU32(base, samplers.color + kTextureDirect3D), fetch)) {
              diffuse_at = fetch[1] & ~0xFFFu;
            }
          }
          KeepFabricDiffuse(diffuse_at & 0x1FFFF000u);  // rn28
          const int64_t signature = int64_t{diffuse_at} * 1000003 + static_cast<int64_t>(frame / 300);
          if (seen.size() < 32 && std::find(seen.begin(), seen.end(), signature) == seen.end()) {
            seen.push_back(signature);
            float kept[3] = {};
            {
              std::lock_guard<std::mutex> lock(g_model_light_mutex);
              const auto it = g_model_lights.find(object);
              if (it != g_model_lights.end()) {
                for (int r = 0; r < 3; ++r) kept[r] = it->second.rows[r][0];
              }
            }
            REXLOG_INFO("cod3: ngl side: rn27 native fabric draw of {:08X} (frame {}): diffuse {:08X} (look set {:08X}, "
                        "frame {} {}); world at {:.1f} {:.1f} {:.1f}; "
                        "{} point light(s): at {:.1f} {:.1f} {:.1f}, 1 / range {:.6f}, color {:.3f} {:.3f} {:.3f}; row "
                        "xx red {:.4f} (the world's xx yy zz red {:.4f} {:.4f} {:.4f}); the sun {:.3f} {:.3f} {:.3f} "
                        "traveling {:.3f} {:.3f} {:.3f} ({}); {} lit draws so far in passes that didn't refill the "
                        "light block",
                        object, frame, diffuse_at, samplers.looks, look_frame.value,
                        look_frame.own ? "its own" : "the scene's", d.world[12], d.world[13], d.world[14],
                        d.point_light_count,
                        d.point_light[0][0], d.point_light[0][1], d.point_light[0][2], d.point_light[0][3],
                        d.point_light[0][4], d.point_light[0][5], d.point_light[0][6], d.irradiance[0][0], kept[0],
                        kept[1], kept[2], d.light_color[0], d.light_color[1], d.light_color[2],
                        d.light_direction[0], d.light_direction[1], d.light_direction[2],
                        fabric_read && fabric_pass_read.sun_bound ? "rn31: as its pass binds it" : "not bound",
                        g_light_block_stale_draws.load(std::memory_order_relaxed));
          }
        }
      }
    }
    std::memcpy(d.view_projection, camera->second.view_projection, sizeof d.view_projection);
    if (camera->second.split) {  // [rexglue-native] 2026-11-09: kept in its player's part of the screen
      d.scissor_x = camera->second.scissor_x;
      d.scissor_y = camera->second.scissor_y;
      d.scissor_width = camera->second.scissor_width;
      d.scissor_height = camera->second.scissor_height;
    }
    if (!g_quiet && draws.size() - first < 2) Probe(base, frame, surface, d);  // not in live mode (every frame)
    // [rexglue-native] 2026-11-09 rn6: and each split view's first surface, to see where its corners land
    if (!g_quiet && camera->second.split && !camera->second.probed) {
      camera->second.probed = true;
      REXLOG_INFO("cod3: ngl side: frame {}: split view {:08X}'s first surface, draw {} of the frame:", frame, s.scene,
                  draws.size());
      Probe(base, frame, surface, d);
    }
    {  // side90: the object's second matrix (+0x40) beside world x the scene's view-projection, to see whether the
       // gun (drawn wider than the game's) has its own projection there
      static int front_matrices = 0, world_matrices = 0;
      int& shown = camera->second.front ? front_matrices : world_matrices;
      if (shown < 2) {
        ++shown;
        std::string own, product;
        char item[24];
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += d.world[4 * r + k] * d.view_projection[4 * k + c];
            std::snprintf(item, sizeof item, " %.4g", g::ReadF32(base, object + 0x40 + 4 * (4 * r + c)));
            own += item;
            std::snprintf(item, sizeof item, " %.4g", sum);
            product += item;
          }
          own += r < 3 ? " |" : "";
          product += r < 3 ? " |" : "";
        }
        REXLOG_INFO("cod3: ngl side: {} object {:08X}: +40:{}; world x view-projection:{}",
                    camera->second.front ? "front" : "world", object, own, product);
      }
    }
    // rn40o: (rn40p: only the watched ones from frame 2900, rn40o's lines went to the ulans' small cube-lit part)
    t_skin_watch = d.lens_cube && d.lens_opaque && REXCVAR_GET(cod3_debug_ngl_side) && frame >= 2900 &&
                   WatchedTextureName(base, samplers.color);
    if (kSkin) d.corners = SkinnedCorners(base, surface, object, frame);
    if (kSkin && skinned && !g_quiet) {  // rn42a (debug only)
      LogNearSkinned(base, object, surface, samplers.color, d, camera->second.view_projection, frame);
    }
    if (t_skin_watch) {  // rn40o: and what the draw ends with (its mesh, its corners, where it is)
      static std::atomic<int> said{0};
      if (said.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_INFO("cod3: ngl side: rn40o helmet draw of object {:08X} (frame {}): mesh {}, corners {}, skinned {}, "
                    "world at {:.1f} {:.1f} {:.1f}",
                    object, frame, d.mesh, d.corners, skinned ? 1 : 0, d.world[12], d.world[13], d.world[14]);
      }
    }
    t_skin_watch = false;
    if (skinned) ProbeFrontSkinned(base, surface, object, d, camera->second.front);  // side93/95: the arms
    if (skinned && camera->second.front && REXCVAR_GET(cod3_debug_ngl_side_arms) > 0) {  // side96-97: the arms
      const int how = REXCVAR_GET(cod3_debug_ngl_side_arms);
      if (how != 2) {
        d.texture = d.lightmap = d.env_texture = d.env_mask = d.normal_map = d.specular_map = 0;
        d.fabric_mask = d.normal_swatch = 0;  // [rexglue-native] rn24
        d.noise_swatch = d.metal_cube = d.metal_scratch = d.detail_map = 0;  // rn32, rn34, rn36
        d.vehicle_bumped = 0;                                                // rn39
      }
      if (how != 3) d.shading = NGL_VK_SHADE_UNLIT;
      static std::atomic<int> said{0};
      if (said.fetch_add(1, std::memory_order_relaxed) < 2) {
        REXLOG_INFO("cod3: ngl side: arms surface {:08X} drawn for the check ({}): texture {}, shading {}, alpha {}, "
                    "lit {}", surface, how, d.texture, d.shading, d.alpha, d.lit);
      }
    }
    // [rexglue-native] 2026-11-10 rn41q: the game's unlit effects (cod3_debug_ngl_side_unlit_effects; rn41i-p, The
    // Mace's lanterns). The glass's effect standard_7_1 draws both its passes with 5DD7AA099EE15C4F: the color map x
    // the corners' colors, its alpha the map's, no light, no fog, blended by that alpha (07060706), no depth written,
    // CullMode CW then CCW (one draw of both sides here). The flames' standard_10 draws one pass of 1DF1E9465C1429B4:
    // the same from the map's 8-frame strip at 24 frames a second (its literals c255 = (24, 0.5, 0.125)): frame
    // floor(c16.x x 24 + 0.5) mod 8, c16 bound to the value block's +0x310, the scene's time (+0x420, the same value
    // in rn41l/p); CullMode None. Both ride in the tint (ngl_vk.h NGL_VK_TINT_*).
    if (REXCVAR_GET(cod3_debug_ngl_side_unlit_effects) && d.texture && !d.lightmap && !d.normal_map &&
        !d.env_texture && !d.lens_cube && !d.metal_cube && !d.fabric_mask && !d.blend_texture) {
      const std::string effect = PassEffect(base, s.node);
      const bool glass = effect == "standard_7_1", flames = effect == "standard_10";
      if (glass || flames) {
        constexpr float kStripRate = 24.0f;  // 1DF1E9465C1429B4's c255.x
        constexpr uint32_t kStripFrames = 8;  // 1 / its c255.z
        d.shading = NGL_VK_SHADE_UNLIT;
        d.lit = 0;
        d.alpha = NGL_VK_ALPHA_BLEND;
        d.blend = NGL_VK_BLEND_ALPHA;
        uint32_t tint = NGL_VK_TINT_EFFECT | NGL_VK_TINT_MAP_ALPHA;
        if (flames) {
          float time[4] = {};
          const PassBinding binding = ReadPassBinding(base, s.node);
          if (!ReadBound(base, binding, 16, time, 1)) {  // its c16, else the scene's time (the same in rn41l/p)
            const uint32_t scene = g::ReadU32(base, kCurrentScene);
            time[0] = Pointer(scene) && GrassReadable(scene + 0x420) ? g::ReadF32(base, scene + 0x420) : 0.0f;
          }
          const double count = std::floor(double{time[0]} * kStripRate + 0.5);
          const uint32_t shown =
              std::isfinite(count) && count >= 0.0 ? static_cast<uint32_t>(std::fmod(count, kStripFrames)) : 0u;
          tint |= kStripFrames | shown << 4;
        }
        d.tint = tint;
        left_out[glass ? "(of those, unlit effects: the lanterns' glass, standard_7_1)"
                       : "(of those, unlit effects: the lanterns' flames, standard_10)"]++;
      }
    }
    if (REXCVAR_GET(cod3_debug_ngl_side) && frame >= kWatchFrom && (samplers.color || samplers.normal)) {  // rn35
      // rn40d: or by its normal map's name, for a color map with none (the Polish infantry's helmets)
      WatchDraw(base, s.node, object, material, samplers.color, samplers.normal, skinned, camera->second.front, d,
                frame);
    }
    // [rexglue-native] 2026-11-10 rn40r: a check (cod3_debug_ngl_side_eyes_show 4): the watched maps' generic bumped
    // draws with a specular map (rn40q: the likely helmet, mesh 421 of the soldier whose eyes are mesh 414) drawn flat
    // and unlit (white), to see their shape and place in the picture (it was the helmet; since rn40t they are taken as
    // 1D4725CEF0551EBB's, with cod3_debug_ngl_side_helmets off they show here again)
    const bool generic_bumped_watched = REXCVAR_GET(cod3_debug_ngl_side) && d.normal_map && !d.fabric_mask &&
                                        !d.noise_swatch && !d.lens_cube && !d.metal_cube && !d.env_texture &&
                                        !d.detail_map && !d.vehicle_bumped && frame >= kWatchFrom &&
                                        WatchedTextureName(base, samplers.color);
    if (generic_bumped_watched) {  // rn40s: their passes' bindings (which of the game's shaders draws them), once each
      static std::mutex mutex;
      static std::vector<uint32_t> logged;
      bool fresh = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (logged.size() < 8 && std::find(logged.begin(), logged.end(), material) == logged.end()) {
          logged.push_back(material);
          fresh = true;
        }
      }
      if (fresh) {
        // rn40u: and why it wasn't taken as a helmet (rn40t: the near soldier's helmet, material ADEA4440, stayed
        // generic though its pass is bound as 1D4725CEF0551EBB's): ReadHelmetPass's why, or the samplers that keep
        // the draw from reaching it
        REXLOG_INFO("cod3: ngl side: rn40s generic bumped watched draw: material {:08X}, object {:08X}, mesh {}, "
                    "specular map {}; rn40u not a helmet: {} (noise swatch {:08X}, detail {:08X}, fabric mask "
                    "{:08X}, normal swatch {:08X})", material, object, d.mesh, d.specular_map,
                    helmet_pass.why[0] ? helmet_pass.why : "its pass not read", samplers.noise_swatch,
                    samplers.detail, samplers.fabric_mask, samplers.normal_swatch);
        LogPassBinding(base, s.node, "rn40s generic bumped watched");
      }
    }
    if (REXCVAR_GET(cod3_debug_ngl_side_eyes_show) == 4 && generic_bumped_watched && d.specular_map) {
      d.texture = d.normal_map = d.specular_map = 0;
      d.lit = 0;
      d.shading = NGL_VK_SHADE_UNLIT;
    }
    if (g_live_shot && d.lit) {  // rn42t (debug only)
      LogLiveLight(frame, object, d, camera->second.view_projection, camera->second.front, light_source, light_kept,
                   sun_source, skinned, block_sun);
    }
    draws.push_back(d);
    if (left_out.on()) {
      char in_scene[96];  // which scenes the draws come from, in what order, to which target
      const auto order = census.scene_order.find(s.scene);
      std::snprintf(in_scene, sizeof in_scene, "(drawn in scene %08X: order %d target %08X near %.2f%s%s)", s.scene,
                    order != census.scene_order.end() ? order->second : -1,
                    g::ReadU32(base, s.scene + kSceneColorTarget), NearPlane(base, s.scene),
                    s.scene == world_scene ? " world" : "", skinned ? " skinned" : "");
      left_out[in_scene]++;
    }
    if (world_lit && samplers.color_b && d.alpha == NGL_VK_ALPHA_OPAQUE && d.lightmap_scale > 0.0f &&
        REXCVAR_GET(cod3_debug_ngl_side_world_second_layer) && draws.size() < kMaxDraws) {
      // side195: the terrain's second layer, drawn again over the first by its corners' weight
      std::string why_b;
      const uint32_t mesh_b = MeshFor(base, surface, why_b, true);
      const uint32_t texture_b = mesh_b ? TextureFor(base, samplers.color_b) : 0;
      if (mesh_b && texture_b) {
        ngl_vk_mesh_draw b = d;
        b.mesh = mesh_b;
        b.texture = texture_b;
        b.alpha = NGL_VK_ALPHA_BLEND;
        b.blend = NGL_VK_BLEND_ALPHA;
        b.corner_alpha = 1;
        draws.push_back(b);
        left_out["(of those, with their second layer mixed in by their corners' weight)"]++;
        static std::atomic<int> said{0};
        if (said.fetch_add(1, std::memory_order_relaxed) < 6) {
          Geometry geo_b;
          std::string unused_b;
          ReadGeometry(base, surface, geo_b, unused_b);
          float u[3] = {}, v[3] = {};
          const uint32_t at = geo_b.vertices + geo_b.vertex_start * geo_b.stride;
          for (int k = 0; k < 3; ++k) VertexUv(base, at, geo_b, k, &u[k], &v[k]);
          REXLOG_INFO("cod3: ngl side: second layer of surface {:08X} (material {:08X}): stride {}, coordinates at "
                      "{} / {} / {}, color at {}; first corner ({:.3f}, {:.3f}) / ({:.3f}, {:.3f}) / ({:.3f}, "
                      "{:.3f}), weight {}", surface, material, geo_b.stride, geo_b.uv[0], geo_b.uv[1], geo_b.uv[2],
                      geo_b.color, u[0], v[0], u[1], v[1], u[2], v[2], SecondLayerWeight(base, at, geo_b) >> 24);
        }
      } else {
        left_out["second layer left out: " + (mesh_b ? std::string("its texture isn't drawable") : why_b)]++;
      }
    }
    if (draws.size() >= kMaxDraws) break;
  }
  TreeDraws(base, census, draws, left_out);
  if (census.far_solids) left_out["(of the surfaces, from the far lists)"] = census.far_solids;
  if (census.far_trees) left_out["(far trees, left out)"] = census.far_trees;
  if (left_out.on()) {
    std::string reasons;
    for (const auto& [why, n] : left_out.counts())
      reasons += (reasons.empty() ? "" : ", ") + why + " x" + std::to_string(n);
    REXLOG_INFO("cod3: ngl side: frame {}: 3D: {} generic surfaces, {} drawn ({} meshes made so far); left out: {}",
                frame, census.solids.size(), draws.size() - first, g_meshes.size(), reasons.empty() ? "none" : reasons);
  }
}
#endif

// Direct3D's device at a draw (docs/COD3_D3D.md, "The device's state shadow"; owner 2026-11-05 12:56: use the
// decompiled Direct3D). The device keeps a copy of the GPU's registers that each draw sends, so at a draw it holds
// everything the GPU will use, constants the game wrote into it directly included: +0x0700 the vertex shader's 256 float
// constants and +0x1700 the pixel shader's, 16 bytes each; +0x284C the registers 0x2100-0x2114, RB_FOG_COLOR (0x2109)
// at +0x2870. No surface's parameter list carries the haze (side35's hunt for "fog" names found none), so at dumps 3
// and 6 the next draws are read on the render job's thread: for each new pair of call site and set of non-zero pixel
// constants, up to kDrawsLogged, the non-zero pixel constants, and for the first kVertexDrawsLogged the vertex ones.
#if defined(COD3_MP)
constexpr uint32_t kDevicePointer = 0x82A8C0FC;  // d3d_mp_twins.tsv
#else
constexpr uint32_t kDevicePointer = 0x8290F24C;
#endif
constexpr uint32_t kDeviceVertexConstants = 0x0700;
constexpr uint32_t kDevicePixelConstants = 0x1700;
constexpr uint32_t kDeviceFogColor = 0x2870;
constexpr uint32_t kDeviceColorMask = 0x285C;  // [rexglue-native] rn42g: RB_COLOR_MASK (0x2104)
constexpr int kDeviceConstants = 256;
constexpr int kConstantsListed = 64;
constexpr int kDrawsRead = 4000;  // about four frames of forest's draws
constexpr int kDrawsLogged = 32;
constexpr int kVertexDrawsLogged = 4;

std::atomic<int> g_draws_to_read{0};
std::mutex g_draw_mutex;
std::unordered_set<uint64_t> g_draw_keys;
int g_draws_logged = 0;

// The device lives in the guest's heap or physical memory; it is in use, so its pages are there.
bool DeviceReadable(uint32_t device) {
  return Pointer(device) && (Readable(device) || (device >= 0x40000000u && device < 0x80000000u));
}

// ---------------------------------------------------------------------------------------------------------------
// The colors as the game's picture is shown (side92; owner 2026-11-05 22:10: "consider the colorspaces and the patch
// we had to apply"). The GPU emulation presents the front buffer through the display's 256-entry gamma table, and the
// game's own screenshot is taken after it. Direct3D's SetGammaRamp (0x822E94D0; cod3mp 0x822B8218) puts the curve of
// the display the system reports (VdGetCurrentDisplayGamma, so kernel_display_gamma_type, which cod3_color_space sets
// through SDK patch 0040; game/color_output.cpp) on top of the game's own ramp (its brightness). It keeps the result in
// the device at +0x3510 (256 big-endian 16-bit values each of red, green and blue), and 0x822FCF50 writes their top 10
// bits to the table. With cod3_color_space bt709, patch 0040's display_output_transfer (bt1886) is folded into the
// presented table too. ngl_vk draws the game's values, so its pictures go through the same table here, the way the
// presenter applies it, before they are saved (the untouched picture is saved as well, "-drawn"). HDR output (patch
// 0044) comes after the table and isn't in the game's screenshot. The display format (+0x33A0) is logged beside the
// table: a 10-bit front buffer would be shown through the PWL table instead, which this doesn't read.
#if defined(COD3_NGL_VK)
constexpr uint32_t kDeviceGammaRamp = 0x3510;
constexpr uint32_t kDeviceDisplayFormat = 0x33A0;

// SDK patch 0040's BT.1886 (gamma 2.4, black at 0) decoded, then encoded as sRGB.
double Bt1886ToSrgb(double v) {
  v = std::clamp(v, 0.0, 1.0);
  const double linear = std::pow(v, 2.4);
  return linear <= 0.0031308 ? 12.92 * linear : 1.055 * v - 0.055;
}

// The presented table, as 8-bit values for each of the game's 8-bit values, per channel.
struct ShownColors {
  std::array<std::array<uint8_t, 256>, 3> table{};
  bool from_device = false;  // false: no device yet, so a plain ramp
  bool bt1886 = false;
  bool identity = true;
  uint32_t display_format = 0;
};

ShownColors ShownColorsNow(uint8_t* base) {
  ShownColors shown;
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  shown.from_device = DeviceReadable(device);
  if (shown.from_device) shown.display_format = g::ReadU32(base, device + kDeviceDisplayFormat);
  shown.bt1886 = rex::cvar::GetFlagInfo("display_output_transfer") &&
                 rex::cvar::GetFlagByName("display_output_transfer") == "bt1886";
  for (int channel = 0; channel < 3; ++channel) {
    for (int i = 0; i < 256; ++i) {
      uint32_t value_10b = static_cast<uint32_t>(std::lround(i * 1023.0 / 255.0));
      if (shown.from_device)
        value_10b = g::ReadU16(base, device + kDeviceGammaRamp + static_cast<uint32_t>(channel * 256 + i) * 2) >> 6;
      if (shown.bt1886) value_10b = static_cast<uint32_t>(std::lround(Bt1886ToSrgb(value_10b / 1023.0) * 1023.0));
      const auto value = static_cast<uint8_t>(std::lround(value_10b * 255.0 / 1023.0));
      shown.table[channel][i] = value;
      shown.identity = shown.identity && value == i;
    }
  }
  return shown;
}

// A copy of an rgba picture through the presented table; the table is logged whenever it changes.
std::vector<uint8_t> AsShown(uint8_t* base, const uint8_t* rgba, uint32_t width, uint32_t height) {
  const ShownColors shown = ShownColorsNow(base);
  const auto& t = shown.table;
  char text[320];
  std::snprintf(text, sizeof(text),
                "%s%s, display format %08X: red 0/32/64/128/192/255 -> %u/%u/%u/%u/%u/%u, green 128 -> %u, "
                "blue 128 -> %u%s",
                shown.from_device ? "the game's" : "no device yet, a plain ramp",
                shown.bt1886 ? ", with BT.1886 shown on sRGB" : "", shown.display_format, t[0][0], t[0][32], t[0][64],
                t[0][128], t[0][192], t[0][255], t[1][128], t[2][128], shown.identity ? " (none: as drawn)" : "");
  static std::mutex logged_mutex;
  static std::string logged;
  {
    std::lock_guard lock(logged_mutex);
    if (logged != text) {
      logged = text;
      REXLOG_INFO("cod3: ngl side: presented gamma table: {}", text);
    }
  }
  std::vector<uint8_t> out(rgba, rgba + static_cast<size_t>(width) * height * 4);
  if (!shown.identity) {
    for (size_t i = 0; i < out.size(); i += 4) {
      for (int c = 0; c < 3; ++c) out[i + c] = t[c][out[i + c]];
    }
  }
  return out;
}

// Saves both: <stem>.ppm as shown (through the presented table, what side_compare.py sets beside the game's own
// screenshot) and <stem>-drawn.ppm as ngl_vk drew it.
void SaveShownPpm(uint8_t* base, const std::filesystem::path& folder, const std::string& stem, const uint8_t* rgba,
                  uint32_t width, uint32_t height) {
  const std::vector<uint8_t> shown = AsShown(base, rgba, width, height);
  SavePpm(folder / (stem + ".ppm"), shown.data(), width, height);
  SavePpm(folder / (stem + "-drawn.ppm"), rgba, width, height);
}
#endif  // COD3_NGL_VK

// The non-zero float constants of a block of kDeviceConstants, as "cN=(x y z w)"; key takes which ones they are.
std::string ConstantsText(uint8_t* base, uint32_t at, uint64_t* key) {
  std::string text;
  char item[96];
  int listed = 0;
  for (int c = 0; c < kDeviceConstants; ++c) {
    const uint32_t a = at + 16u * static_cast<uint32_t>(c);
    if (!(g::ReadU32(base, a) | g::ReadU32(base, a + 4) | g::ReadU32(base, a + 8) | g::ReadU32(base, a + 12))) continue;
    if (key) *key = (*key ^ static_cast<uint64_t>(c + 1)) * 1099511628211ull;
    if (++listed > kConstantsListed) continue;
    std::snprintf(item, sizeof item, " c%d=(%.4g %.4g %.4g %.4g)", c, g::ReadF32(base, a), g::ReadF32(base, a + 4),
                  g::ReadF32(base, a + 8), g::ReadF32(base, a + 12));
    text += item;
  }
  if (listed > kConstantsListed) text += " ... (" + std::to_string(listed) + " in all)";
  return text.empty() ? std::string(" none") : text;
}

void ReadNextDraws() {
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  g_draw_keys.clear();
  g_draws_logged = 0;
  g_draws_to_read.store(kDrawsRead, std::memory_order_relaxed);
}

uint64_t g_frames = 0;
int g_dumps = 0;
bool g_listed = false;

#if defined(COD3_NGL_VK)
std::vector<ngl_vk_mesh_draw> SkyDraws(uint8_t* base, uint64_t frame);
std::vector<ngl_vk_mesh_draw> EffectDraws(uint8_t* base, uint64_t frame, const ngl_vk_mesh_draw* fog_from);
std::vector<ngl_vk_mesh_draw> RainDraws(uint8_t* base, uint64_t frame);  // rn16

// side108: the sun's shadow. The game draws three orthographic maps around the camera every frame (0x8242BAA0: 384,
// 1024 and 2048 units wide, far plane 5000; dev/agents/shadows/NOTES.md), with the soldiers, props and trees in them
// (trees in a scene without a perspective camera, TreeDraws' count); a model's first light, the sun, comes through
// them, so under the trees the soldiers keep little more than their ambient (side106: the light sets' sun is the
// same 0.627 0.568 0.446 everywhere; side106off, `cod3_shadow_quality=off`, lights them). Natively one map,
// kShadowWidth units wide, centered kShadowAhead ahead of the camera along the ground, kShadowDepth each side along
// the sun's way; the casters and the shadowed are the world scene's draws lit by the sun (soldiers, props, trees),
// not the lightmapped world (its sun is in its lightmaps) and nothing blended.
constexpr float kShadowWidth = 3072.0f;
constexpr float kShadowAhead = 1200.0f;
constexpr float kShadowDepth = 4000.0f;
constexpr float kShadowTexels = 2048.0f;  // ngl_vk's map, a side

struct SunShadow {
  bool ok = false;
  float light[16];   // world to the light's clip space (Direct3D row vectors)
  float camera[16];  // the view-projection of the shadowed draws
};

bool Inverse4(const float m[16], double out[16]) {
  double a[16], cofactor[16], determinant = 0.0;
  for (int i = 0; i < 16; ++i) a[i] = m[i];
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      double minor[9];
      int n = 0;
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          if (r != row && c != column) minor[n++] = a[r * 4 + c];
        }
      }
      cofactor[row * 4 + column] = ((row + column) & 1 ? -1.0 : 1.0) *
                                   (minor[0] * (minor[4] * minor[8] - minor[5] * minor[7]) -
                                    minor[1] * (minor[3] * minor[8] - minor[5] * minor[6]) +
                                    minor[2] * (minor[3] * minor[7] - minor[4] * minor[6]));
    }
  }
  for (int column = 0; column < 4; ++column) determinant += a[column] * cofactor[column];
  if (!(std::fabs(determinant) > 1e-30)) return false;
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) out[row * 4 + column] = cofactor[column * 4 + row] / determinant;
  }
  return true;
}

SunShadow ShadowFor(std::vector<ngl_vk_mesh_draw>& draws, uint64_t frame) {
  SunShadow s;
  if (!REXCVAR_GET(cod3_debug_ngl_side_shadow)) return s;
  // side169: the scene's sun (COD_SHADOW_POSITION, the place the game's own shadow cascades look from) first; the last
  // light set's main light only without it. Black Burn's last set was a tank's fixed light (0, -0.707, -0.707) while
  // the sun travels (0.433, 0.250, -0.866), so its shadows fell the wrong way.
  const float* way = g_sun.ok ? g_sun.direction : g_sun_way_known ? g_sun_way : nullptr;
  if (!way) return s;
  // the world's camera: the view-projection most of the sun-lit draws share
  std::map<std::array<float, 16>, int> counts;
  for (const ngl_vk_mesh_draw& d : draws) {
    if (d.shading != NGL_VK_SHADE_LIGHT || d.alpha == NGL_VK_ALPHA_BLEND) continue;
    std::array<float, 16> vp;
    std::memcpy(vp.data(), d.view_projection, sizeof d.view_projection);
    counts[vp]++;
  }
  if (counts.empty()) return s;
  const auto best = std::max_element(counts.begin(), counts.end(),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
  std::memcpy(s.camera, best->first.data(), sizeof s.camera);
  double inverse[16];
  if (!Inverse4(s.camera, inverse)) return s;
  // the eye is where clip x, y and w are 0: clip (0, 0, 1, 0) back into the world, the inverse's third row; the view's
  // middle far away is clip (0, 0, 1, 1), the third and fourth rows
  const double* e = inverse + 8;
  const double far_w = inverse[11] + inverse[15];
  if (std::fabs(e[3]) < 1e-12 || std::fabs(far_w) < 1e-12) return s;
  double eye[3], ahead[3];
  for (int i = 0; i < 3; ++i) {
    eye[i] = e[i] / e[3];
    ahead[i] = (inverse[8 + i] + inverse[12 + i]) / far_w - eye[i];
  }
  ahead[2] = 0.0;  // along the ground (z is up)
  const double ahead_length = std::sqrt(ahead[0] * ahead[0] + ahead[1] * ahead[1]);
  double center[3] = {eye[0], eye[1], eye[2]};
  if (ahead_length > 1e-6) {
    for (int i = 0; i < 2; ++i) center[i] += ahead[i] / ahead_length * kShadowAhead;
  }
  // the light's frame: forward the way the sun travels, right and up across it
  double f[3] = {way[0], way[1], way[2]};
  const double f_length = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  if (!(f_length > 1e-6)) return s;
  for (double& v : f) v /= f_length;
  const double up0[3] = {std::fabs(f[2]) < 0.99 ? 0.0 : 1.0, 0.0, std::fabs(f[2]) < 0.99 ? 1.0 : 0.0};
  double r[3] = {up0[1] * f[2] - up0[2] * f[1], up0[2] * f[0] - up0[0] * f[2], up0[0] * f[1] - up0[1] * f[0]};
  const double r_length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  for (double& v : r) v /= r_length;
  const double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
  // the center snapped to whole texels across the map, so the shadows don't crawl as the camera moves
  const double texel = kShadowWidth / kShadowTexels, half = kShadowWidth / 2.0;
  const double cr = std::round((center[0] * r[0] + center[1] * r[1] + center[2] * r[2]) / texel) * texel;
  const double cu = std::round((center[0] * u[0] + center[1] * u[1] + center[2] * u[2]) / texel) * texel;
  const double cf = center[0] * f[0] + center[1] * f[1] + center[2] * f[2];
  for (int i = 0; i < 3; ++i) {
    s.light[i * 4 + 0] = static_cast<float>(r[i] / half);
    s.light[i * 4 + 1] = static_cast<float>(u[i] / half);
    s.light[i * 4 + 2] = static_cast<float>(f[i] / (2.0 * kShadowDepth));
    s.light[i * 4 + 3] = 0.0f;
  }
  s.light[12] = static_cast<float>(-cr / half);
  s.light[13] = static_cast<float>(-cu / half);
  s.light[14] = static_cast<float>((kShadowDepth - cf) / (2.0 * kShadowDepth));
  s.light[15] = 1.0f;
  int marked = 0, ground = 0;
  const bool world_shadowed = REXCVAR_GET(cod3_debug_ngl_side_world_shadow);
  for (ngl_vk_mesh_draw& d : draws) {
    if ((d.alpha == NGL_VK_ALPHA_BLEND && !d.corner_alpha) ||  // side195: the second layer is shadowed as the first
        std::memcmp(d.view_projection, s.camera, sizeof s.camera) != 0)
      continue;
    if (d.shading == NGL_VK_SHADE_LIGHTMAP && d.lightmap_scale > 0.0f && world_shadowed) {
      d.sun_shadowed = 1;  // side165: the lightmapped world takes the shadow, its sun x it (the shaders' DEPTHMAP kin)
      ++ground;
      continue;
    }
    if (d.shading != NGL_VK_SHADE_LIGHT) continue;
    d.shadow_caster = 1;
    d.sun_shadowed = 1;
    ++marked;
  }
  s.ok = marked > 0;
  if (!g_quiet || g_live_shot)  // rn42t: and the live frames saved (debug only)
    REXLOG_INFO("cod3: ngl side: frame {}: the sun's shadow: {} draws cast and take it, {} lightmapped take it; eye "
                "({:.0f} {:.0f} {:.0f}), map center ({:.0f} {:.0f} {:.0f}), the sun traveling {:.3f} {:.3f} {:.3f}",
                frame, marked, ground, eye[0], eye[1], eye[2], center[0], center[1], center[2], f[0], f[1], f[2]);
  return s;
}
#endif

#if defined(COD3_NGL_VK)
// side120: the native picture in the game's window. It is handed to the presenter as the guest output, as the
// emulated GPU's swap does (Presenter::RefreshGuestOutput), so the window shows it with everything the presenter adds
// (its scaling and effects, the overlays). The refresher copies the picture, as shown (through the game's gamma
// table, as AsShown), into the guest output image (A2B10G10R10, each 8-bit value widened) on the presenter's Vulkan
// device: an upload buffer and one copy on the graphics queue, waited for before the buffer is filled again. Only
// with the plugin's host backend (cod3_native_vulkan), which draws nothing itself: beside the emulated GPU the two
// would hand the presenter pictures at once.
struct Feed {
  bool failed = false;
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint32_t memory_type = UINT32_MAX;
  uint32_t* mapped = nullptr;
  VkDeviceSize bytes = 0;
  bool pending = false;  // the fence waits on the last copy
  uint64_t shown = 0;
  // side121: since the last live line (OnListSend): pictures handed over, pictures dropped because the last one was
  // still being painted, and the game thread's time in Show
  uint64_t handed = 0, dropped = 0;
  double show_ms = 0.0;
};
Feed g_feed;

// side121: the presenter paints the picture in the thread that hands it over (its paint mode), and in side120b that
// held the game's thread for about a third of a second a frame (3 fps; 60 with the native side drawing but not handed
// over, side120d). So the tail of each hand-over (Presenter::RefreshGuestOutput's defer_tail: the completion signal,
// the mailbox and the paint) runs on a thread of its own, and a picture that comes while the last one is still being
// painted isn't handed over: the window drops it, the game never waits for the window.
class Painter {
 public:
  bool Busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
  }
  // Runs tail on the painting thread (started the first time)
  void Run(rex::ui::Presenter::GuestOutputRefreshTail tail) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_) {
        std::thread([this] { Loop(); }).detach();  // for the program's life (see g_painter)
        started_ = true;
      }
      tail_ = std::move(tail);
      busy_ = true;
    }
    wake_.notify_one();
  }
  // The painting time since the last call: total ms and paints
  std::pair<double, uint64_t> TakeTimes() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::pair<double, uint64_t> times{paint_ms_, paints_};
    paint_ms_ = 0.0;
    paints_ = 0;
    return times;
  }

 private:
  void Loop() {
    for (;;) {
      rex::ui::Presenter::GuestOutputRefreshTail tail;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return static_cast<bool>(tail_); });
        tail = std::move(tail_);
        tail_ = nullptr;
      }
      const auto start = std::chrono::steady_clock::now();
      tail();
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      std::lock_guard<std::mutex> lock(mutex_);
      paint_ms_ += ms;
      ++paints_;
      busy_ = false;
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  rex::ui::Presenter::GuestOutputRefreshTail tail_;
  bool busy_ = false, started_ = false;
  double paint_ms_ = 0.0;
  uint64_t paints_ = 0;
};
Painter* g_painter = new Painter;  // never destroyed: its thread may still be painting when the program ends

// The upload buffer and the rest, made once for the presenter's device (false: they couldn't be)
bool FeedReady(Feed& f, const rex::ui::vulkan::VulkanDevice* device, VkDeviceSize bytes) {
  if (f.device) return f.device == device && f.bytes >= bytes;
  const auto& dfn = device->functions();
  const VkDevice vk_device = device->device();
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;  // begun again each frame
  pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(vk_device, &pool_info, nullptr, &f.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate_info.commandPool = f.pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(vk_device, &allocate_info, &f.commands) != VK_SUCCESS) return false;
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (dfn.vkCreateFence(vk_device, &fence_info, nullptr, &f.fence) != VK_SUCCESS) return false;
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = bytes;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(vk_device, &buffer_info, nullptr, &f.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements;
  dfn.vkGetBufferMemoryRequirements(vk_device, f.buffer, &requirements);
  f.memory_type =
      rex::ui::vulkan::util::ChooseHostMemoryType(device->memory_types(), requirements.memoryTypeBits, false);
  if (f.memory_type == UINT32_MAX) return false;
  VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  memory_info.allocationSize = requirements.size;
  memory_info.memoryTypeIndex = f.memory_type;
  if (dfn.vkAllocateMemory(vk_device, &memory_info, nullptr, &f.memory) != VK_SUCCESS) return false;
  if (dfn.vkBindBufferMemory(vk_device, f.buffer, f.memory, 0) != VK_SUCCESS) return false;
  void* mapped = nullptr;
  if (dfn.vkMapMemory(vk_device, f.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
  f.mapped = static_cast<uint32_t*>(mapped);
  f.device = device;
  f.bytes = bytes;
  return true;
}

// The window's presenter, and its Vulkan device in device when the window is drawn by Vulkan (else nullptr)
rex::ui::Presenter* WindowPresenter(const rex::ui::vulkan::VulkanDevice*& device) {
  auto* kernel = rex::system::kernel_state();
  auto* runtime = kernel ? kernel->emulator() : nullptr;
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  auto* provider = graphics ? dynamic_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider()) : nullptr;
  device = provider ? provider->vulkan_device() : nullptr;
  return graphics ? graphics->presenter() : nullptr;
}

// Shows width x height rgba pixels (ngl_vk's target) in the game's window
void Show(uint8_t* base, const uint8_t* rgba, uint32_t width, uint32_t height) {
  if (g_feed.failed || !rgba || !width || !height) return;
  if (g_painter->Busy()) {  // side121: the last picture is still being painted
    ++g_feed.dropped;
    return;
  }
  const auto show_start = std::chrono::steady_clock::now();
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  rex::ui::Presenter* presenter = WindowPresenter(device);
  const VkDeviceSize bytes = VkDeviceSize{width} * height * 4;
  if (!presenter || !device || !FeedReady(g_feed, device, bytes)) {
    g_feed.failed = true;
    REXLOG_WARN("cod3: ngl side: the native picture can't be shown in the window ({})",
                !presenter ? "no presenter" : !device ? "the presenter isn't Vulkan's" : "no upload buffer");
    return;
  }
  const auto& dfn = device->functions();
  const VkDevice vk_device = device->device();
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };
  const Clock::time_point t0 = Clock::now();  // side122: where the game's thread spends its time here
  Clock::time_point copy_start{}, copy_end{};
  if (g_feed.pending) {  // the last copy is done before its buffer is filled again
    dfn.vkWaitForFences(vk_device, 1, &g_feed.fence, VK_TRUE, UINT64_MAX);
    dfn.vkResetFences(vk_device, 1, &g_feed.fence);
    g_feed.pending = false;
  }
  const Clock::time_point t1 = Clock::now();
  const ShownColors shown = ShownColorsNow(base);
  const auto widen = [](uint32_t v) { return v << 2 | v >> 6; };  // 8 bits to 10
  // side209: each channel's 10-bit word, in place, made once a frame (768 entries), so a pixel is three look-ups and
  // three ORs; the loop was 1.4-1.7 ms of the game's main thread a frame (side208's hand-over lines)
  uint32_t words[3][256];
  for (uint32_t channel = 0; channel < 3; ++channel) {
    for (uint32_t i = 0; i < 256; ++i) words[channel][i] = widen(shown.table[channel][i]) << (10 * channel);
  }
  constexpr uint32_t kOpaque = 3u << 30;
  const size_t pixels = size_t{width} * height;
  uint32_t* out = g_feed.mapped;
  for (size_t i = 0; i < pixels; ++i) {
    uint32_t rgba_word;
    std::memcpy(&rgba_word, rgba + 4 * i, 4);  // bytes r, g, b, a: little-endian, r in the low byte
    out[i] = words[0][rgba_word & 0xFF] | words[1][(rgba_word >> 8) & 0xFF] | words[2][(rgba_word >> 16) & 0xFF] |
             kOpaque;
  }
  rex::ui::vulkan::util::FlushMappedMemoryRange(device, g_feed.memory, g_feed.memory_type);
  const Clock::time_point t2 = Clock::now();
  const bool refreshed = presenter->RefreshGuestOutput(
      width, height, 16, 9, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vulkan_context =
            static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        copy_start = Clock::now();
        context.SetIs8bpc(true);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dfn.vkBeginCommandBuffer(g_feed.commands, &begin) != VK_SUCCESS) return false;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;  // all of it is written: what was there goes (after the presenter's last read)
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vulkan_context.image();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dfn.vkCmdPipelineBarrier(g_feed.commands,
                                 vulkan_context.image_ever_written_previously()
                                     ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        dfn.vkCmdCopyBufferToImage(g_feed.commands, g_feed.buffer, vulkan_context.image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;  // then as the presenter reads it
        barrier.dstAccessMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
        dfn.vkCmdPipelineBarrier(g_feed.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
                                 nullptr, 1, &barrier);
        if (dfn.vkEndCommandBuffer(g_feed.commands) != VK_SUCCESS) return false;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g_feed.commands;
        auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
        if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, g_feed.fence) != VK_SUCCESS) return false;
        g_feed.pending = true;
        copy_end = Clock::now();
        return true;
      },
      [](rex::ui::Presenter::GuestOutputRefreshTail tail) { g_painter->Run(std::move(tail)); });
  ++g_feed.handed;
  const Clock::time_point t3 = Clock::now();
  g_feed.show_ms += ms(show_start, t3);
  // side122: the first hand-overs and then one in 120, in parts (side124: with the dumps only)
  if (REXCVAR_GET(cod3_debug_ngl_side) && (g_feed.shown < 12 || g_feed.shown % 120 == 0)) {
    REXLOG_INFO("cod3: ngl side: hand-over {}: {:.2f} ms: before the fence {:.2f}, the fence {:.2f}, filling the "
                "buffer {:.2f}, the presenter {:.2f} (before the copy {:.2f}, the copy {:.2f}, after it {:.2f})",
                g_feed.shown + 1, ms(show_start, t3), ms(show_start, t0), ms(t0, t1), ms(t1, t2), ms(t2, t3),
                copy_start.time_since_epoch().count() ? ms(t2, copy_start) : -1.0,
                copy_end.time_since_epoch().count() ? ms(copy_start, copy_end) : -1.0,
                copy_end.time_since_epoch().count() ? ms(copy_end, t3) : -1.0);
  }
  if (++g_feed.shown == 1 || (!refreshed && g_feed.shown < 5)) {
    REXLOG_INFO("cod3: ngl side: the native picture {} in the window ({}x{}, through the game's gamma table)",
                refreshed ? "shown" : "not taken by the presenter", width, height);
  }
}

// side210: ngl_vk on the presenter's own device (cod3_native_shared_device). It submits to the presenter's graphics
// queue, which the SDK guards with a mutex (VulkanDevice::AcquireQueue), so ngl_vk holds it around each use: taken and
// let go on the same thread.
thread_local std::optional<rex::ui::vulkan::VulkanDevice::Queue::Acquisition> t_queue_hold;

void* HoldQueue(void* user) {
  const auto* device = static_cast<const rex::ui::vulkan::VulkanDevice*>(user);
  t_queue_hold.emplace(device->AcquireQueue(device->queue_family_graphics_compute(), 0));
  return t_queue_hold->queue();
}

void ReleaseQueue(void*) { t_queue_hold.reset(); }

bool g_shared_device = false;  // ngl_vk draws on the presenter's device and hands its pictures over on the GPU
int g_gpu_hand_over_failures = 0;

// A Vulkan handle as the 64-bit value ngl_vk.h takes (a pointer on 64-bit builds)
template <typename Handle>
uint64_t HandleBits(Handle handle) {
  if constexpr (std::is_pointer_v<Handle>) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
  } else {
    return static_cast<uint64_t>(handle);
  }
}

// [rexglue-native] 2026-11-09: the native picture's size for this start: cod3_native_resolution when it names one, else
// what the game itself is drawn at for the window (the render plan), else the window; 16:9, from 640x360 to 7680x4320.
void ChooseTargetSize() {
  uint32_t width = 0, height = 0;
  const char* from = "cod3_native_resolution";
  const std::string asked = REXCVAR_GET(cod3_native_resolution);
  if (asked.empty() || !cod3::ParseResolutionChoice(asked, &width, &height)) {
    if (!asked.empty())
      REXLOG_WARN("cod3: ngl side: cod3_native_resolution \"{}\" isn't a size (720p, 1080p, 1440p, 2160p or WxH); "
                  "the game's own size is used",
                  asked);
    const cod3::RenderPlan plan = cod3::GetRenderPlan();
    width = plan.output_width();
    height = plan.output_height();
    from = "the size the game is drawn at";
    if (!width || !height) {  // "original" (the game's own table entry) or no plan yet: the window's
      width = plan.display_width;
      height = plan.display_height;
      from = "the window";
    }
  }
  if (!width || !height) {
    width = 1280;
    height = 720;
    from = "nothing better known";
  }
  cod3::FitSixteenByNine(&width, &height);
  if (width > 7680 || height > 4320) {
    width = 7680;
    height = 4320;
  }
  if (width < 640 || height < 360) {
    width = 640;
    height = 360;
  }
  g_target_width = width;
  g_target_height = height;
  REXLOG_INFO("cod3: ngl side: the native picture is drawn at {}x{} ({})", width, height, from);
}

// Starts ngl_vk: on the presenter's device with cod3_native_shared_device (on its own device if that fails), else on
// its own. Returns 0 when it is ready.
int StartNglVk() {
  // [rexglue-native] 2026-11-09: ngl_vk opens the system's Vulkan loader itself (it no longer links one, so Windows
  // builds have it too): which one, or that there is none
  const char* loader = ngl_vk_loader();
  REXLOG_INFO("cod3: ngl side: ngl_vk's Vulkan loader: {}", loader ? loader : "none found (ngl_vk can't start)");
  if (!loader) return -1;
  ChooseTargetSize();  // [rexglue-native] 2026-11-09
  ngl_vk_set_geometry_on_gpu(REXCVAR_GET(cod3_native_gpu_geometry) ? 1 : 0);  // side220
  if (g_native.load(std::memory_order_relaxed) && REXCVAR_GET(cod3_native_shared_device)) {
    const rex::ui::vulkan::VulkanDevice* device = nullptr;
    WindowPresenter(device);
    if (device && ngl_vk_init_on_device(g_target_width, g_target_height, device->vulkan_instance()->instance(),
                                        device->physical_device(), device->device(),
                                        device->queue_family_graphics_compute(), 0, HoldQueue, ReleaseQueue,
                                        const_cast<void*>(static_cast<const void*>(device))) == 0) {
      g_shared_device = true;
      REXLOG_INFO("cod3: ngl side: ngl_vk draws on the window's own Vulkan device; pictures go to it on the GPU");
      return 0;
    }
    REXLOG_WARN("cod3: ngl side: ngl_vk can't draw on the window's Vulkan device ({}); it uses one of its own",
                device ? "ngl_vk_init_on_device failed" : "the window isn't drawn by Vulkan");
  }
  return ngl_vk_init(g_target_width, g_target_height);
}

// side210: the hand-over on the GPU (ngl_vk on the presenter's device): in the refresher, ngl_vk draws its target into
// the guest output image through the game's gamma table (ngl_vk_output), submitted on the same queue after the
// frame's draw. Nothing goes to the CPU and back, and nothing is filled on the game's thread.
void ShowOnGpu(uint8_t* base) {
  if (g_feed.failed) return;
  if (g_painter->Busy()) {  // as in Show: the last picture is still being painted
    ++g_feed.dropped;
    return;
  }
  using Clock = std::chrono::steady_clock;
  using VulkanPresenter = rex::ui::vulkan::VulkanPresenter;
  const Clock::time_point show_start = Clock::now();
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  rex::ui::Presenter* presenter = WindowPresenter(device);
  if (!presenter || !device) {
    g_feed.failed = true;
    REXLOG_WARN("cod3: ngl side: the native picture can't be shown in the window ({})",
                !presenter ? "no presenter" : "the presenter isn't Vulkan's");
    return;
  }
  const ShownColors shown = ShownColorsNow(base);
  uint32_t table[256];  // red in the low byte, as ngl_vk_output takes it
  for (uint32_t i = 0; i < 256; ++i) {
    table[i] = uint32_t{shown.table[0][i]} | uint32_t{shown.table[1][i]} << 8 | uint32_t{shown.table[2][i]} << 16 |
               0xFF000000u;
  }
  bool called = false, drawn = false;
  const bool refreshed = presenter->RefreshGuestOutput(
      g_target_width, g_target_height, 16, 9, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vulkan_context = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        called = true;
        context.SetIs8bpc(true);
        const uint32_t after = vulkan_context.image_ever_written_previously()
                                   ? uint32_t{VulkanPresenter::kGuestOutputInternalStageMask}
                                   : 0u;  // never used: nothing to wait for
        drawn = ngl_vk_output(HandleBits(vulkan_context.image()), HandleBits(vulkan_context.image_view()),
                              VulkanPresenter::kGuestOutputFormat, g_target_width, g_target_height, after,
                              VulkanPresenter::kGuestOutputInternalLayout,
                              VulkanPresenter::kGuestOutputInternalStageMask,
                              VulkanPresenter::kGuestOutputInternalAccessMask, table) == 0;
        return drawn;
      },
      [](rex::ui::Presenter::GuestOutputRefreshTail tail) { g_painter->Run(std::move(tail)); });
  ++g_feed.handed;
  if (called && !drawn && ++g_gpu_hand_over_failures == 3) {  // then through the CPU, as before (Show), from the next frame
    g_shared_device = false;
    REXLOG_WARN("cod3: ngl side: the hand-over on the GPU failed 3 times; pictures go through the CPU again");
  }
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - show_start).count();
  g_feed.show_ms += ms;
  if (REXCVAR_GET(cod3_debug_ngl_side) && (g_feed.shown < 12 || g_feed.shown % 120 == 0)) {
    REXLOG_INFO("cod3: ngl side: hand-over {} on the GPU: {:.2f} ms of the game's thread{}", g_feed.shown + 1, ms,
                drawn ? "" : " (ngl_vk_output failed)");
  }
  if (++g_feed.shown == 1 || (!refreshed && g_feed.shown < 5)) {
    REXLOG_INFO("cod3: ngl side: the native picture {} in the window ({}x{}, handed over on the GPU, through the "
                "game's gamma table)",
                refreshed ? "shown" : "not taken by the presenter", g_target_width, g_target_height);
  }
}
#endif

#if defined(COD3_NGL_VK)
// side125: the game's depth of field and its menus' blur, both mixed in by ngl_vk after the 3D and under the quads
// (ngl_vk_set_depth_of_field); the scenes drawn in front of the world (the gun, the arms, cod3mp's side-selection
// soldier; squeezed into kFrontDepth) stay sharp, as the game draws them after its post effects. From the CoD3
// decompile (campaign; cod3mp's are 0x17EA8 further on):
//  - Depth of field: the end of the scene (0x82426500) draws its pass (0x82149C80) while the player is aimed down
//    the sights (0x8253D290: at least 95% of the way; cod3mp 0x824311E0: while fewer than two players share the
//    screen, 0x82BCC878), and the pass draws while its state is on (post_effects.cpp's addresses): it copies the
//    scene, shrinks and blurs it (0x82147608, 0x82147458) and mixes the blur back by depth, from the near distance to
//    the far one. The player's cod3_depth_of_field (post_effects.cpp's) turns it off.
//  - The menus' blur: the end of the scene's 0x82420DE8 (cod3mp 0x8242F390) queues post effect 10 with an amount
//    that fades in while a menu is up (its state 2 or 3); effect 10 (0x8214A908) shrinks and blurs the scene, blurs
//    it twice more, and mixes it over the whole scene by that amount, depth or not.
#if defined(COD3_MP)
constexpr uint32_t kDofOn = 0x82A8F7D4, kDofNear = 0x825C3410, kDofFar = 0x825C3414;
constexpr uint32_t kLocalPlayers = 0x82BCC878;
constexpr uint32_t kMenuBlurState = 0x82BCC8C0, kMenuBlurAmount = 0x82BCC8B0;  // + 4 x the view's player (0 here)
#else
constexpr uint32_t kDofOn = 0x829128C4, kDofNear = 0x825B3070, kDofFar = 0x825B3074;
constexpr uint32_t kEntityList = 0x82A2AB40, kEntityClient = 0x210, kSightsFraction = 0x8C;
constexpr float kAimedFraction = 0.95f;
constexpr uint32_t kMenuBlurState = 0x82A2ABE4, kMenuBlurAmount = 0x82A2ABE0;
#endif

// Whether the game draws its depth of field pass this frame (before its own on/off): the campaign while aimed down
// the sights (as post_effects.cpp reads it), cod3mp while one player has the screen.
bool DepthOfFieldPassDrawn(uint8_t* base) {
#if defined(COD3_MP)
  return g::ReadU32(base, kLocalPlayers) < 2;
#else
  const uint32_t player = g::ReadU32(base, cod3::addr::kActiveLocalClient);
  const uint32_t list = g::ReadU32(base, kEntityList);
  if (player > 3 || !Pointer(list)) return false;
  const uint32_t entity = g::ReadU32(base, list + 4 * (player + 1));
  if (!Pointer(entity)) return false;
  const uint32_t client = g::ReadU32(base, entity + kEntityClient);
  if (!Pointer(client)) return false;
  const float aimed = g::ReadF32(base, client + kSightsFraction);
  return std::isfinite(aimed) && aimed >= kAimedFraction;
#endif
}

// side127: the menus' blur as the game draws it: post effect 10's own function (campaign 0x8214A908, cod3mp
// 0x821627B0; hooked at the end of this file) called with its amount since the last frame. The menus' state can't
// say: a menu sets it to its last step (4, amount 1) every frame, and the end of the scene draws the blur and clears
// the state before the native side reads it (side126: state 0 on cod3mp's side selection, which the game blurs).
std::atomic<uint32_t> g_menu_blur_amount{0};  // the float's bits
std::atomic<uint32_t> g_menu_blur_calls{0};

float MenuBlurThisFrame(uint64_t frame) {
  static uint64_t frame_seen = ~uint64_t{0};
  static uint32_t calls_seen = 0;
  static float blur = 0.0f;
  static int missed = 0;
  if (frame != frame_seen) {  // Dump asks twice in a frame with pictures
    frame_seen = frame;
    const uint32_t calls = g_menu_blur_calls.load(std::memory_order_acquire);
    if (calls != calls_seen) {
      const uint32_t bits = g_menu_blur_amount.load(std::memory_order_relaxed);
      float amount;
      std::memcpy(&amount, &bits, sizeof amount);
      blur = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
      missed = 0;
    } else if (++missed > 2) {  // a frame can slip between the game's call and this one (side128: after a long frame)
      blur = 0.0f;
    }
    calls_seen = calls;
  }
  return blur;
}

}  // namespace

// From the hook on post effect 10's function: the amount it was drawn with.
void KeepMenuBlur(float amount) {
  uint32_t bits;
  std::memcpy(&bits, &amount, sizeof bits);
  g_menu_blur_amount.store(bits, std::memory_order_relaxed);
  g_menu_blur_calls.fetch_add(1, std::memory_order_release);
}

namespace {
// side130: every post effect the game asks for, from its dispatcher (campaign 0x8214B0B0, cod3mp 0x82162F58; hooked at
// the end of this file): r3 the type, r4 its 0x30 bytes of parameters (the first word 1: on). For now only counted and
// logged with the pictures, to see which ones each level uses (0 glow, 1 tint, 3 color grading, 4 motion blur, 7 film,
// 10 the menus' blur; side127's switches).
constexpr uint32_t kPostEffectTypes = 12, kPostEffectWords = 12;
struct PostEffectSeen {
  std::atomic<uint32_t> calls{0};
  uint32_t words[kPostEffectWords] = {};
};
PostEffectSeen g_post_effects[kPostEffectTypes];

void LogPostEffects(uint8_t* base, uint64_t frame) {
  (void)base;
  static uint32_t logged[kPostEffectTypes] = {};
  std::string text;
  for (uint32_t type = 0; type < kPostEffectTypes; ++type) {
    const uint32_t calls = g_post_effects[type].calls.load(std::memory_order_acquire);
    if (calls == logged[type]) continue;
    char item[160];
    const uint32_t* w = g_post_effects[type].words;
    float f[6];
    std::memcpy(f, w + 1, sizeof f);
    std::snprintf(item, sizeof item, "%s%u x%u (%08X; %.3g %.3g %.3g %.3g %.3g %.3g)", text.empty() ? "" : ", ", type,
                  calls - logged[type], w[0], f[0], f[1], f[2], f[3], f[4], f[5]);
    text += item;
    logged[type] = calls;
  }
  REXLOG_INFO("cod3: ngl side: frame {}: the game's post effects since the last picture: {}", frame,
              text.empty() ? "none" : text);
}
}  // namespace

// side170: the apsColorUVANode draws (see cod3_debug_skip_color_uv_nodes), on the render job's threads: counted, and
// the first few nodes of each kind of mesh logged. Their layout, from 0x820FCD70 and its 0x820FD788: +0x10..+0x4F a
// matrix (the Render's 0x8210FFE8 takes it when the mesh's +0x6C is 0), +0x50 the instance's position, +0x60 the color (the mesh's color, times the instance's), +0x70 / +0x74 / +0x78 the instance's own vertex
// data (start, count, size), +0x7C flags (bit 3: past the fog), +0xB0 the mesh (its +0x30 / +0x34 the blend,
// +0x60 / +0x64 the texture coordinates' motion, +0x80..+0x8C four values passed to the shader).
std::atomic<uint32_t> g_color_uv_draws{0};

// side179: whether the game draws a node of the kind with this bit of cod3_debug_skip_game_draws
bool KeepGameDraw(uint32_t bit) {
  return (static_cast<uint32_t>(REXCVAR_GET(cod3_debug_skip_game_draws)) & bit) == 0;
}

bool KeepColorUvNode(uint8_t* base, uint32_t node) {
  g_color_uv_draws.fetch_add(1, std::memory_order_relaxed);
  if (Watching() && Pointer(node)) {
    static std::mutex lock;
    static std::unordered_set<uint32_t> shown;
    const uint32_t mesh = g::ReadU32(base, node + 0xB0);
    std::lock_guard<std::mutex> hold(lock);
    if (shown.size() < 8 && shown.insert(mesh).second) {
      std::string words;
      for (uint32_t i = 0; i < 0xA0 && Pointer(mesh); i += 4) {
        char item[12];
        std::snprintf(item, sizeof item, "%s%08X", i % 0x10 ? " " : (i ? " | " : ""), g::ReadU32(base, mesh + i));
        words += item;
      }
      REXLOG_INFO("cod3: ngl side: an apsColorUVANode {:08X}: at ({:.1f} {:.1f} {:.1f}), its matrix's last row ({:.1f} "
                  "{:.1f} {:.1f}), color ({:.3f} {:.3f} {:.3f} {:.3f}), flags {:08X}, own vertices {:08X} x{} of {} "
                  "bytes; its mesh {:08X}: {}",
                  node, g::ReadF32(base, node + 0x50), g::ReadF32(base, node + 0x54), g::ReadF32(base, node + 0x58),
                  g::ReadF32(base, node + 0x40), g::ReadF32(base, node + 0x44), g::ReadF32(base, node + 0x48),
                  g::ReadF32(base, node + 0x60), g::ReadF32(base, node + 0x64), g::ReadF32(base, node + 0x68),
                  g::ReadF32(base, node + 0x6C), g::ReadU32(base, node + 0x7C), g::ReadU32(base, node + 0x70),
                  g::ReadU32(base, node + 0x74), g::ReadU32(base, node + 0x78), mesh, words);
    }
  }
  return !REXCVAR_GET(cod3_debug_skip_color_uv_nodes);
}

// [rexglue-native] 2026-11-09 rn13: the game's draws counted by call site (the return address of Direct3D's draw),
// with cod3_debug_ngl_side, and listed with each picture, to find the draws the native side has no counterpart for
// (The Mace's rain: not its effect flushes, apsColorUVANode draws or the two node kinds of side179).
// cod3_debug_skip_game_call_site leaves one call site's draws out of the game's own picture.
// rn43e: the counter is built without ngl_vk too (OnDraw counts in every build; only the listing needs ngl_vk).
#endif  // COD3_NGL_VK
std::mutex g_draw_sites_lock;
std::unordered_map<uint32_t, uint32_t> g_draw_sites;

void CountDrawSite(uint32_t call_site) {
  std::lock_guard<std::mutex> hold(g_draw_sites_lock);
  if (g_draw_sites.size() < 4096 || g_draw_sites.count(call_site)) ++g_draw_sites[call_site];
}
#if defined(COD3_NGL_VK)  // rn43e

// The call sites since the last call, in address order: " <site> x<draws>" each
std::string TakeDrawSites() {
  std::vector<std::pair<uint32_t, uint32_t>> sites;
  {
    std::lock_guard<std::mutex> hold(g_draw_sites_lock);
    sites.assign(g_draw_sites.begin(), g_draw_sites.end());
    g_draw_sites.clear();
  }
  std::sort(sites.begin(), sites.end());
  std::string text;
  for (const auto& site : sites) {
    char item[32];
    std::snprintf(item, sizeof item, " %08X x%u", site.first, site.second);
    text += item;
  }
  return text;
}

bool SkipGamePostEffect(uint32_t type) {  // side200
  return type < 31 && ((static_cast<uint32_t>(REXCVAR_GET(cod3_debug_skip_game_post_effects)) >> type) & 1) != 0;
}

namespace {
void SaveShader(uint8_t* base, uint32_t shader, uint32_t header_at, uint32_t block_at, bool vertex,
                const std::string& prefix);

// [rexglue-native] 2026-11-10 rn41b: post effect 7's time, stepped 1/30 at each of its calls by the dispatcher's branch
// for it (campaign 0x8214B0B0; cod3mp's 0x82162F58 has the same branch, globals_sp_to_mp.tsv), kept as its last call
// found it (the float's bits) for Film
#if defined(COD3_MP)
constexpr uint32_t kPostEffect7Time = 0x82A8FA08;
#else
constexpr uint32_t kPostEffect7Time = 0x82912AF8;
#endif
std::atomic<uint32_t> g_film_time{0};

#if !defined(COD3_MP)
// side201: post effect 3, as the dispatcher's branch for it draws it (campaign 0x8214B0B0): texture stage 1 is one of
// the game's screen copies, *(kScreenCopies + 4 x *kScreenCopyIndex) (0x822EBE40), then the full-screen pass 0x8214AC90
// with the shader object *kPostEffect3Shader and no constants. Its parameters (0, 1280, 720, 0, 1.0, a pointer on
// Island) aren't read. Brightness (type 2) is the same pass with *kPostEffect2Shader and two constants.
constexpr uint32_t kPostEffect3Shader = 0x829125D0, kPostEffect2Shader = 0x829125D8;
constexpr uint32_t kScreenCopies = 0x82912AE8, kScreenCopyIndex = 0x829128D4;

// Once, while watching: both shaders saved for ps_names.py (ngl-side-postfx3-ps-*.bin, ngl-side-postfx2-ps-*.bin) and
// the screen copy's texture object logged, to name post effect 3's shader and see what it reads.
void ProbePostEffect3(uint8_t* base) {
  static std::atomic<bool> probed{false};
  if (!Watching() || probed.exchange(true)) return;
  SaveShader(base, g::ReadU32(base, kPostEffect3Shader), 0x28, 0x18, false, "ngl-side-postfx3");
  SaveShader(base, g::ReadU32(base, kPostEffect2Shader), 0x28, 0x18, false, "ngl-side-postfx2");
  const uint32_t index = g::ReadU32(base, kScreenCopyIndex);
  const uint32_t texture = index < 16 ? g::ReadU32(base, kScreenCopies + 4 * index) : 0;
  REXLOG_INFO("cod3: ngl side: post effect 3: stage 1 is screen copy {} ({:08X}): {}", index, texture,
              Pointer(texture) ? Words(base, texture, 0x10) : std::string("not a pointer"));
}

// [rexglue-native] 2026-11-10 rn41b: post effect 7, as the dispatcher's branch for it draws it (decomp 0x8214B0B0):
// two full-screen passes from the screen copy (0x8214AEA0, stage 0 clamped, stage 1 wrapped). First the shader object
// *kPostEffect7First with c0 = (words 4, 5, 6, a time stepped 1/30 at each call, kPostEffect7Time), c1 = words 7-10,
// c2 = (word 11 three times, 1); then *kPostEffect7Second with c0 = 1 - word 1 (all four), c1 = word 1, c2 = (words 2,
// 3, 0, 0). rn9's dump of The Mace has a second shader that fits (D7B45AAE96CFCD55: out = tf0(uv) x c0 + tf1(uv +
// c2.xy) x c1, a copy of the picture moved by words 2 and 3 mixed in by word 1) and a first one that may
// (470878A4FB9D8A6F: taps of the picture around each pixel at random places c0.x apart, a grain mixed in by c0.y, then
// mixed by c0.z toward the gray of (color x c2) times c1). Seen with the pictures (side130): about 215 calls in 600
// frames, words 1-6 (0.5, two small offsets, 0.003, 0, 0.44); in a shellshock word 5 0.108. A check before drawing it
// natively (debug only): both shaders saved once (ngl-side-postfx7a/b) and all its words with the frames it comes on:
// the first 16 calls from frame 2990, and up to 12 calls whose word 5 isn't 0.
constexpr uint32_t kPostEffect7First = 0x829125B8, kPostEffect7Second = 0x829125BC;

void ProbePostEffect7(uint8_t* base, uint32_t parameters) {
  if (!REXCVAR_GET(cod3_debug_ngl_side) || !Watching()) return;
  static std::atomic<bool> saved{false};
  if (!saved.exchange(true)) {
    SaveShader(base, g::ReadU32(base, kPostEffect7First), 0x28, 0x18, false, "ngl-side-postfx7a");
    SaveShader(base, g::ReadU32(base, kPostEffect7Second), 0x28, 0x18, false, "ngl-side-postfx7b");
  }
  static std::atomic<int> early{0}, strong{0};
  const bool grain = g::ReadF32(base, parameters + 20) != 0.0f;
  std::atomic<int>& count = grain ? strong : early;
  if ((!grain && g_frames < 2990) || count.fetch_add(1, std::memory_order_relaxed) >= (grain ? 12 : 16)) return;
  std::string words;
  char item[24];
  for (uint32_t i = 1; i < 12; ++i) {
    std::snprintf(item, sizeof item, " %.4g", g::ReadF32(base, parameters + 4 * i));
    words += item;
  }
  REXLOG_INFO("cod3: ngl side: rn41b post effect 7 at frame {} (word 0 {:08X}): words 1-11:{}; its time {:.4f}",
              g_frames, g::ReadU32(base, parameters), words, g::ReadF32(base, kPostEffect7Time));
}
#endif
}  // namespace

void KeepPostEffect(uint8_t* base, uint32_t type, uint32_t parameters) {
  if (type >= kPostEffectTypes || !Pointer(parameters)) return;
  PostEffectSeen& seen = g_post_effects[type];
  for (uint32_t i = 0; i < kPostEffectWords; ++i) seen.words[i] = g::ReadU32(base, parameters + 4 * i);
  seen.calls.fetch_add(1, std::memory_order_release);
  if (type == 7) g_film_time.store(g::ReadU32(base, kPostEffect7Time), std::memory_order_relaxed);  // rn41b
#if !defined(COD3_MP)
  if (type == 3) ProbePostEffect3(base);
  if (type == 7) ProbePostEffect7(base, parameters);  // rn41b
#endif
  static std::atomic<int> calls_seen{0};  // side202: the order the game asks for its post effects in, once mid-level
  if (Watching()) {
    const int k = calls_seen.fetch_add(1, std::memory_order_relaxed);
    if (k >= 3000 && k < 3012) REXLOG_INFO("cod3: ngl side: post effect call {}: type {}", k, type);
  }
}

namespace {

// side132: post effect `type` as the game asked for it this frame (the dispatcher's hook): its parameters' words, or
// null when it wasn't (held through two missed frames, as the menus' blur: a call can slip past a frame)
const uint32_t* PostEffectThisFrame(uint32_t type, uint64_t frame) {
  struct Seen {
    uint64_t frame = ~uint64_t{0};
    uint32_t calls = 0;
    int missed = 3;
    uint32_t words[kPostEffectWords] = {};
  };
  static Seen seen[kPostEffectTypes];
  if (type >= kPostEffectTypes) return nullptr;
  Seen& s = seen[type];
  if (frame != s.frame) {
    s.frame = frame;
    const uint32_t calls = g_post_effects[type].calls.load(std::memory_order_acquire);
    if (calls != s.calls) {
      std::memcpy(s.words, g_post_effects[type].words, sizeof s.words);
      s.missed = 0;
    } else if (s.missed <= 2) {
      ++s.missed;
    }
    s.calls = calls;
  }
  return s.missed <= 2 && s.words[0] != 0 ? s.words : nullptr;
}

// side132: post effect 1, the tint the campaign draws every frame (the forest: 25% toward gray 0.78). Its pixel shader
// (corpus shader_85F6DFC029EC912F; constants c0 "index" the amount, c1 "orange" the color, c2 "exposure", and the literal
// c255 (0.11, 0.30, 0.59), dotted with blue, red, green): out = mix(c, dot(c x exposure, (0.30, 0.59, 0.11)) x color,
// amount). Its parameters: on, amount, exposure, color red, green, blue (the dispatcher, campaign 0x8214B0B0).
void Tint(uint64_t frame) {
  const uint32_t* words = PostEffectThisFrame(1, frame);
  if (!words) return;
  float f[5];  // amount, exposure, red, green, blue
  std::memcpy(f, words + 1, sizeof f);
  for (const float v : f)
    if (!std::isfinite(v)) return;
  const float amount = std::clamp(f[0], 0.0f, 1.0f);
  if (amount <= 0.0f) return;
  const float weights[3] = {0.30f * f[1], 0.59f * f[1], 0.11f * f[1]};
  const float color[3] = {f[2], f[3], f[4]};
  ngl_vk_set_tint(amount, weights, color, kFrontDepth);
}

// side135: post effect 2, a brightness the campaign ramps as a level starts (the forest: 72 frames, to 0.969). Its
// pixel shader (shader_377AC015A488B262, the one the dispatcher hands from 0x829125D8): out = (c x c0.y) ^ c0.x, alpha
// kept, with c0 = its first two parameters (the power, then the brightness).
void Brightness(uint64_t frame) {
  const uint32_t* words = PostEffectThisFrame(2, frame);
  if (!words) return;
  float f[2];  // the power, the brightness
  std::memcpy(f, words + 1, sizeof f);
  if (!std::isfinite(f[0]) || !std::isfinite(f[1])) return;
  const float power = std::clamp(f[0], 0.1f, 10.0f), brightness = std::clamp(f[1], 0.0f, 4.0f);
  if (power == 1.0f && brightness == 1.0f) return;
  ngl_vk_set_brightness(brightness, power, kFrontDepth);
  static int shown = 0;
  if (shown < 6 && (frame % 8 == 0 || shown == 0)) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: frame {}: the brightness {:.3f}, to the power {:.3f}", frame, brightness, power);
  }
}

// Post effect 4, a motion blur (shader_1A3598F9A2CE3A59: this frame x (1 - a) + the last frame x a, a = its first
// word; the game keeps the last frame's picture in its own target): ngl_vk blends the last frame it drew.
void MotionBlur(uint64_t frame) {
  const uint32_t* words = PostEffectThisFrame(4, frame);
  if (!words) return;
  float amount;
  std::memcpy(&amount, words + 1, sizeof amount);
  if (!std::isfinite(amount) || amount <= 0.0f) return;
  amount = std::min(amount, 0.95f);
  ngl_vk_set_motion_blur(amount, kFrontDepth);
  static int shown = 0;
  if (shown < 6 && (frame % 8 == 0 || shown == 0)) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: frame {}: the motion blur keeps {:.3f} of the last frame", frame, amount);
  }
}

// [rexglue-native] 2026-11-10 rn41b: post effect 7 (ProbePostEffect7 has its two passes; rn41b's probe of The Mace: it
// starts at an explosion nearby, words 4-6 and 11 rising over five calls to 0.01, 0.27, 1 and 4.36, then fading over
// seconds; words 7-9 0.515 0.54 0.535 throughout): the taps' spread (word 4), the grain (5), how far toward the gray
// (6), the gray's color (7-9) and the scale before it (11), with its time; then the picture moved by words 2 and 3 and
// mixed in by word 1 (0.5). Drawn by ngl_vk after the motion blur (as the game, after post effect 4), over the whole
// 3D: in the game's picture a concussion blurs and doubles the gun too (rn41d), while its crosshair stays sharp (rn41g).
void Film(uint64_t frame) {
  if (!REXCVAR_GET(cod3_debug_ngl_side_film)) return;
  const uint32_t* words = PostEffectThisFrame(7, frame);
  if (!words) return;
  float w[kPostEffectWords];
  std::memcpy(w, words, sizeof w);  // (word 0 is the on switch, not a float)
  for (uint32_t i = 1; i < kPostEffectWords; ++i)
    if (!std::isfinite(w[i])) return;
  float time = 0.0f;
  const uint32_t time_bits = g_film_time.load(std::memory_order_relaxed);
  std::memcpy(&time, &time_bits, sizeof time);
  if (!std::isfinite(time)) time = 0.0f;
  const float first[4] = {std::clamp(w[4], 0.0f, 0.1f), std::clamp(w[5], 0.0f, 1.0f), std::clamp(w[6], 0.0f, 1.0f),
                          time};
  const float gray[3] = {w[7], w[8], w[9]};
  const float move[2] = {std::clamp(w[2], -1.0f, 1.0f), std::clamp(w[3], -1.0f, 1.0f)};
  ngl_vk_set_film(first, gray, std::clamp(w[11], 0.0f, 16.0f), move, std::clamp(w[1], 0.0f, 1.0f), kFrontDepth);
  static int shown = 0;
  if (shown < 6 && (frame % 8 == 0 || shown == 0)) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: frame {}: rn41b post effect 7 natively: spread {:.4f}, grain {:.3f}, toward the gray "
                "{:.3f} (scale {:.3f}), moved {:.4f} {:.4f} by {:.2f}, time {:.3f}",
                frame, first[0], first[1], first[2], w[11], move[0], move[1], w[1], time);
  }
}

#if !defined(COD3_MP)
// side202: post effect 3 natively. Its pixel shader (shader_8B289D78B550EBBD, named in side201) takes red and green
// from the table on texture stage 1 at (red, green), blue from it at (blue, red), alpha kept. The table is the screen
// copy the dispatcher puts on stage 1 (kScreenCopies, a 64 x 64 8888 texture on Island), decoded here and made an
// ngl_vk texture again whenever its picture changes (at most kLookupTables of them: they are never freed).
constexpr int kLookupTables = 64;

void ColorLookup(uint8_t* base, uint64_t frame) {
  if (!PostEffectThisFrame(3, frame) || (!REXCVAR_GET(cod3_debug_ngl_side_color_lookup) && !Watching())) return;
  static uint64_t last_sum = 0;
  static uint32_t last_id = 0;
  static int made = 0, failed = 0;
  static uint32_t forgets_seen = 0;  // side228: after ngl_vk_forget the table is made again
  if (forgets_seen != g_forgets) {
    forgets_seen = g_forgets;
    last_sum = 0;
    last_id = 0;
    made = 0;
  }
  const uint32_t index = g::ReadU32(base, kScreenCopyIndex);
  const uint32_t texture = index < 16 ? g::ReadU32(base, kScreenCopies + 4 * index) : 0;
  uint32_t fetch[6];
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why = "no texture object";
  if (!Pointer(texture) || !ReadFetch(base, texture, fetch)) {
    if (failed++ < 4) REXLOG_INFO("cod3: ngl side: frame {}: post effect 3's table {:08X} not read: {}", frame, texture, why);
    return;
  }
  // side204: side203's table (fetch E5245086: in the 0xE0000000 view) came out with its rows turned by 16, one page:
  // that view sits 4 KB into physical memory (the SDK's GetPhysicalAddress adds 0x1000 for it), so the game's own
  // writes through it are a page further on than the fetch constant's address.
  const bool e_view = (fetch[1] >> 29) == 7u;
  const int32_t pages = std::clamp(REXCVAR_GET(cod3_debug_ngl_side_lookup_pages), 0, 4);
  if (e_view) fetch[1] += static_cast<uint32_t>(pages) * 0x1000u;
  if (!DecodeFetch(base, fetch, bgra, width, height, why)) {
    if (failed++ < 4) REXLOG_INFO("cod3: ngl side: frame {}: post effect 3's table {:08X} not read: {}", frame, texture, why);
    return;
  }
  if (!width || !height || bgra.size() < 4ull * width * height) return;
  const uint64_t sum = Xxh64(bgra.data(), bgra.size());
  if (sum != last_sum && made < kLookupTables) {
    last_sum = sum;
    last_id = ngl_vk_texture(bgra.data(), width, height);
    ++made;
    if (Watching() && made <= 2) {  // side203: the table as decoded, for reading its channels on the PC
      const std::filesystem::path path = ScreenshotFolder() / ("ngl-side-lookup-" + std::to_string(made) + ".bgra");
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
      REXLOG_INFO("cod3: ngl side: post effect 3's table saved to {} ({}x{} bgra); fetch {:08X} {:08X} {:08X} {:08X} "
                  "{:08X} {:08X}", path.string(), width, height, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4],
                  fetch[5]);
    }
    uint64_t mean[4] = {};
    for (size_t i = 0; i + 3 < bgra.size(); i += 4)
      for (int k = 0; k < 4; ++k) mean[k] += bgra[i + k];
    const uint64_t n = std::max<uint64_t>(1, bgra.size() / 4);
    const auto texel = [&bgra](uint32_t i) {
      uint32_t v = 0;
      std::memcpy(&v, bgra.data() + 4 * static_cast<size_t>(i), sizeof v);
      return v;
    };
    REXLOG_INFO("cod3: ngl side: frame {}: post effect 3's table {:08X} ({}x{}, format {}) is new (#{}): mean blue {} "
                "green {} red {} alpha {}; corners (bgra) {:08X} {:08X} {:08X} {:08X}",
                frame, texture, width, height, fetch[1] & 0x3F, made, mean[0] / n, mean[1] / n, mean[2] / n,
                mean[3] / n, texel(0), texel(width - 1), texel(width * (height - 1)),
                texel(width * height - 1));
    // side204: green down the first column (an identity table goes 0 at the top to 255 at the bottom)
    const auto green = [&](uint32_t row) { return bgra[4 * static_cast<size_t>(std::min(row, height - 1)) * width + 1]; };
    REXLOG_INFO("cod3: ngl side: post effect 3's table #{}: {} page(s) on ({} view); green at x 0, rows 0/15/16/47/48/63: "
                "{} {} {} {} {} {}", made, e_view ? pages : 0, e_view ? "0xE0000000" : "other", green(0), green(15),
                green(16), green(47), green(48), green(63));
  }
  if (last_id && REXCVAR_GET(cod3_debug_ngl_side_color_lookup)) ngl_vk_set_color_lookup(last_id, kFrontDepth);
}
#endif

void DepthOfField(uint8_t* base, uint64_t frame, bool pictures) {
  Tint(frame);  // side132: drawn after the depth of field, as the end of the game's scene does
  Brightness(frame);
#if !defined(COD3_MP)
  ColorLookup(base, frame);  // side202: in the tint's pass, after the brightness
#endif
  MotionBlur(frame);
  Film(frame);  // rn41b: in the tint's pass, after the motion blur
  const bool on = g::ReadU32(base, kDofOn) != 0 && DepthOfFieldPassDrawn(base);
  const float near_distance = g::ReadF32(base, kDofNear), far_distance = g::ReadF32(base, kDofFar);
  // side208: the player's switch, looked up by name at most once every 60 frames instead of every frame
  static bool wanted = true;
  static uint64_t looked = UINT64_MAX;
  if (looked == UINT64_MAX || frame < looked || frame - looked >= 60) {
    looked = frame;
    wanted = !rex::cvar::GetFlagInfo("cod3_depth_of_field") ||
             rex::cvar::GetFlagByName("cod3_depth_of_field") != "false";
  }
  const bool dof = on && wanted && g_world_depth.ok && std::isfinite(near_distance) && std::isfinite(far_distance) &&
                   far_distance > 0.0f;
  const uint32_t menu_state = g::ReadU32(base, kMenuBlurState);
#if defined(COD3_MP)
  // [rexglue-native] 2026-11-09 rn6: with two or more players sharing the screen the game still calls post effect 10
  // (rn4's 4-player mp_merv: amount 1.00 from about frame 3000), yet its picture shows no blur in any player's part;
  // ngl_vk's blanket would cover the whole picture, so it is left out then
  const bool shared_screen = g::ReadU32(base, kLocalPlayers) >= 2;
#else
  const bool shared_screen = false;
#endif
  const float asked = shared_screen ? 0.0f : MenuBlurThisFrame(frame);
  const float blanket = std::max(asked, 0.01f * static_cast<float>(REXCVAR_GET(cod3_debug_ngl_side_blur)));
  float near_depth = 2.0f, far_depth = 2.0f;  // above 1: no depth of field (the blanket alone)
  if (dof) {
    auto forward = [](float d) { return g_world_depth.reversed ? 1.0f - d : d; };
    near_depth = std::clamp(forward(DepthAt(g_world_depth, std::max(near_distance, 0.001f))), 0.0f, 1.0f);
    far_depth = std::clamp(forward(DepthAt(g_world_depth, std::max(far_distance, near_distance + 1.0f))), 0.0f,
                           1.0f);
  }
  if (dof || blanket > 0.0f) ngl_vk_set_depth_of_field(near_depth, far_depth, kFrontDepth, blanket);
  static int said = -1;  // the last state logged: a line when it changes, and at each dump
  const int state = (dof ? 1 : 0) | (blanket > 0.0f ? 2 : 0);
  if (pictures || said != state) {
    said = state;
    REXLOG_INFO("cod3: ngl side: frame {}: depth of field {} (the game's {}, near {:.1f} far {:.1f}{}{}); the menus' "
                "blur {:.2f} (the game's {:.2f}; its state {}, {:.2f})", frame, dof ? "drawn" : "not drawn",
                on ? "on" : "off", near_distance, far_distance, wanted ? "" : ", cod3_depth_of_field off",
                g_world_depth.ok ? "" : ", no world camera", blanket, asked, menu_state,
                g::ReadF32(base, kMenuBlurAmount));
    if (dof && pictures)
      REXLOG_INFO("cod3: ngl side: frame {}: depth of field depths {:.5f} to {:.5f}, kept under {:.3f}", frame,
                  near_depth, far_depth, kFrontDepth);
#if defined(COD3_MP)
    // side127: the other post effects' switches the end of cod3mp's scene reads (0x824311E0), for what the
    // side-selection blur is: depth of field on, local players, tint, color grading (3), motion blur, film
    if (pictures)
      REXLOG_INFO("cod3: ngl side: frame {}: effects: depth of field {} (players {}), tint {:.2f}, grading {:.2f}, "
                  "motion blur {:.2f}, film {}; menus' blur states {} {} {} {}", frame, g::ReadU32(base, kDofOn),
                  g::ReadU32(base, kLocalPlayers), g::ReadF32(base, 0x82BCC918), g::ReadF32(base, 0x82BCC8E0),
                  g::ReadF32(base, 0x82BCC928), g::ReadU32(base, 0x82BCC8E8), g::ReadU32(base, kMenuBlurState),
                  g::ReadU32(base, kMenuBlurState + 4), g::ReadU32(base, kMenuBlurState + 8),
                  g::ReadU32(base, kMenuBlurState + 12));
#endif
  }
  if (pictures) {
    LogPostEffects(base, frame);
    static uint32_t logged = 0;
    const uint32_t draws = g_color_uv_draws.load(std::memory_order_relaxed);
    REXLOG_INFO("cod3: ngl side: frame {}: {} apsColorUVANode draws by the game since the last picture{}", frame,
                draws - logged, REXCVAR_GET(cod3_debug_skip_color_uv_nodes) ? " (left out of its picture)" : "");
    logged = draws;
    // rn13
    REXLOG_INFO("cod3: ngl side: frame {}: the game's draws by call site since the last picture:{}", frame,
                TakeDrawSites());
  }
}
#endif

// side212: where the live frames' time goes, in ms summed since the last live line. Only the thread drawing the live
// frames touches it (the game's main thread, or the worker with cod3_native_worker).
struct FrameParts {
  double scene = 0.0, sky = 0.0, solids = 0.0, effects = 0.0, shadow_and_grass = 0.0, quads = 0.0, drawing = 0.0,
         hand_over = 0.0;
  // side229 [acct2-bro] 2026-11-08: what the frames held, summed between lines (the scene's solids, the solid draws
  // made from them, every draw with the sky and the effects)
  uint64_t scene_solids = 0, solid_draws = 0, all_draws = 0;
};
FrameParts g_parts;

// side119: pictures false is a live frame (cod3_ngl_side_live): drawn, but nothing saved or logged. root is the root
// scene as nglListSend found it (side212: read before the game's nglListSend clears it).
void Dump(uint8_t* base, uint32_t root, uint64_t frame, bool pictures = true) {
  auto part_at = std::chrono::steady_clock::now();
  const auto lap = [&](double& part) {  // side212: adds the time since the last lap to part, on live frames
    if (pictures) return;
    const auto now = std::chrono::steady_clock::now();
    part += std::chrono::duration<double, std::milli>(now - part_at).count();
    part_at = now;
  };
  std::vector<Quad> quads;
  Census census;
  TakeScene(base, root, 0, quads, census);
  lap(g_parts.scene);
  const std::string name = "ngl-side-" + std::to_string(frame);
  g_quiet = !pictures;
  g_live_shot = !pictures && LiveShotFrame(frame);  // rn42j
  if (pictures)
    REXLOG_INFO("cod3: ngl side: frame {}: {} scenes ({} drawn off screen), {} quads, {} strings ({} glyphs), {} other "
                "nodes ({})",
                frame, census.scenes, census.offscreen_scenes, census.quads, census.strings, census.glyphs,
                census.other_nodes, OthersText(base, census));
  // the first dump, and the first with a dozen quads
  if (pictures && (g_dumps == 1 || (!g_listed && census.quads >= 12))) {
    g_listed = g_listed || census.quads >= 12;
    List(base, frame, quads, census);
  }
#if defined(COD3_NGL_VK)
  if (g_state == 0) {
    g_state = StartNglVk() == 0 ? 1 : -1;
    REXLOG_INFO("cod3: ngl side: ngl_vk {} ({})", g_state == 1 ? "started" : "failed to start", ngl_vk_device_name());
  }
  if (g_state != 1) return;
  // side210: on the presenter's device the target goes to the window on the GPU, so it is copied out to the CPU only
  // for the pictures saved on dump frames
  ngl_vk_set_readback(!g_shared_device || pictures || g_live_shot);  // rn42j: and the live frames saved
  std::vector<ngl_vk_mesh_draw> draws = SkyDraws(base, frame);  // the sky first, under everything (side85)
  lap(g_parts.sky);
  const size_t first_solid = draws.size();
  SolidDraws(base, frame, census, draws);  // side213: appended, not copied
  lap(g_parts.solids);
  if (!pictures) {  // side229
    g_parts.scene_solids += census.solids.size();
    g_parts.solid_draws += draws.size() - first_solid;
  }
  if (pictures) {  // side182
    REXLOG_INFO("cod3: ngl side: frame {}: lightmapped world objects with a light set: {}, of those with lights that "
                "aren't directional: {}", frame, g_world_sets.exchange(0), g_world_point_sets.exchange(0));
  }
  {  // side104: the effects last, fogged as the first fogged world draw
    const ngl_vk_mesh_draw* fog_from = nullptr;
    for (size_t i = first_solid; i < draws.size(); ++i) {
      if (draws[i].fog[1] > draws[i].fog[0]) {
        fog_from = &draws[i];
        break;
      }
    }
    const std::vector<ngl_vk_mesh_draw> effects = EffectDraws(base, frame, fog_from);
    draws.insert(draws.end(), effects.begin(), effects.end());
    // [rexglue-native] 2026-11-09 rn16: the rain over them
    const std::vector<ngl_vk_mesh_draw> rain = RainDraws(base, frame);
    draws.insert(draws.end(), rain.begin(), rain.end());
  }
  lap(g_parts.effects);
  if (!pictures) g_parts.all_draws += draws.size();  // side229
  const SunShadow shadow = ShadowFor(draws, frame);  // side108
  const GrassFrame grass = GrassFor(base, census);
  lap(g_parts.shadow_and_grass);
  // ngl_vk draws the 3D first and the quads over it, so quads of the scenes drawn before the first scene with 3D
  // (the frame's clearing quad, say) are left out when there is 3D: in the game they are under it.
  int first_3d = 1 << 30;
  // [rexglue-native] 2026-11-09 rn6: split screen: each player's 3D comes right after that player's own clearing quad
  // (rn4's 4-player mp_merv: an opaque black 320x240 quad in the scene before each player's 3D). Only the first
  // player's was left out (before the first 3D), so the other three covered their players' views: black quarters
  // under the HUD. A quad whose middle lies in the part of a split view drawn after it is under that view's 3D too.
  struct SplitArea {
    int order;
    float part[4];  // SplitPart's
  };
  std::vector<SplitArea> split_areas;
  uint32_t last_scene = 0;
  for (const Solid& s : census.solids) {
    const auto order = census.scene_order.find(s.scene);
    if (order != census.scene_order.end()) first_3d = std::min(first_3d, order->second);
    if (s.scene == last_scene || order == census.scene_order.end()) continue;
    last_scene = s.scene;
    SplitArea area;
    area.order = order->second;
    bool known = false;
    for (const SplitArea& a : split_areas) known = known || a.order == area.order;
    if (!known && SplitPart(base, s.scene, area.part)) split_areas.push_back(area);
  }
  std::vector<ngl_vk_vertex> vertices;
  vertices.reserve(quads.size() * 6);
  int textured = 0, under = 0, backdrops = 0;
  for (const Quad& q : quads) {
    const auto order = census.scene_order.find(q.scene);
    bool under_split = false;  // rn6
    if (!draws.empty() && order != census.scene_order.end() && !split_areas.empty()) {
      const float mx = 0.25f * (q.x[0] + q.x[1] + q.x[2] + q.x[3]) / kScreenWidth;
      const float my = 0.25f * (q.y[0] + q.y[1] + q.y[2] + q.y[3]) / 480.0f;  // the 640x480 screen
      for (const SplitArea& a : split_areas) {
        under_split = under_split || (a.order > order->second && mx >= a.part[0] && mx <= a.part[2] &&
                                      my >= a.part[1] && my <= a.part[3]);
      }
    }
    if (!draws.empty() && order != census.scene_order.end() && (order->second < first_3d || under_split)) {
      if (pictures && under < 6) {  // side145 (diagnostics): which
        REXLOG_INFO("cod3: ngl side: frame {}: quad under the 3D left out: ({:.1f}, {:.1f}) to ({:.1f}, {:.1f}), color "
                    "{:08X}, scene {:08X} (order {}, the 3D from {}), texture {}",
                    frame, q.x[0], q.y[0], q.x[3], q.y[3], q.color[0], q.scene, order->second, first_3d,
                    TextureText(base, q.texture));
      }
      ++under;
      continue;
    }
    // side118: an untextured, opaque black quad over the whole screen is a backdrop the game's depth test keeps
    // behind the 3D (frame 1200's: one at z 0 in its own scene, one at z -9630 under the HUD); ngl_vk draws the 2D
    // over the 3D without depth, so with 3D it is left out (it had turned the native picture with the HUD black)
    const bool whole_screen = std::min(q.x[0], q.x[2]) <= 0.0f && std::max(q.x[1], q.x[3]) >= kScreenWidth &&
                              std::min(q.y[0], q.y[1]) <= 0.0f && std::max(q.y[2], q.y[3]) >= 480.0f;  // 640x480
    if (!draws.empty() && whole_screen && !q.texture &&
        (q.color[0] | q.color[1] | q.color[2] | q.color[3]) == 0xFF000000u &&
        (q.color[0] & q.color[1] & q.color[2] & q.color[3]) == 0xFF000000u) {
      ++backdrops;
      continue;
    }
    const uint32_t texture = TextureFor(base, q.texture);
    textured += texture != 0;
    AddQuad(vertices, q, texture);
  }
  if (under && pictures) REXLOG_INFO("cod3: ngl side: frame {}: {} quads under the 3D left out", frame, under);
  if (backdrops && pictures)
    REXLOG_INFO("cod3: ngl side: frame {}: {} black backdrop quads behind the 3D left out", frame, backdrops);
  {  // side176: the color the 3D starts from (cod3_debug_ngl_side_clear)
    const int32_t clear = REXCVAR_GET(cod3_debug_ngl_side_clear);
    const uint32_t scene = census.world_scene;
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    if (clear && Pointer(scene) && Readable(scene + kSceneFogColor + 12)) {
      const uint32_t from = clear == 2 ? (scene + kSceneFogColor) & ~0xFu : scene + kSceneClearColor;
      for (uint32_t i = 0; i < 3; ++i) {
        rgb[i] = g::ReadF32(base, from + 4 * i);
        if (!std::isfinite(rgb[i])) rgb[i] = 0.0f;
      }
    }
    ngl_vk_set_clear_color(rgb);
    if (pictures) LogClears(base, frame, census);
    if (pictures && clear)
      REXLOG_INFO("cod3: ngl side: frame {}: the 3D starts from {:.3f} {:.3f} {:.3f} (scene {:08X}'s {})", frame, rgb[0],
                  rgb[1], rgb[2], scene, clear == 2 ? "fog color" : "clear color");
  }
  lap(g_parts.quads);
  if (!draws.empty() && pictures) {  // the 3D alone first, then under the 2D
    static bool listed = false;
    if (!listed) {
      listed = true;
      List(base, frame, quads, census);
    }
    const ngl_vk_vertex none{};
    ngl_vk_set_meshes(draws.data(), static_cast<uint32_t>(draws.size()));
    ngl_vk_set_sun_shadow(shadow.ok ? shadow.light : nullptr, shadow.ok ? shadow.camera : nullptr);
    SetGrass(grass);
    DepthOfField(base, frame, false);
    uint32_t width = 0, height = 0;
    const uint8_t* pixels = nullptr;
    if (ngl_vk_draw_quads(&none, 0, kScreenWidth, kScreenHeight, nullptr, 0, 0, 0) == 0 &&
        (pixels = ngl_vk_target_pixels(&width, &height)) != nullptr) {
      SaveShownPpm(base, ScreenshotFolder(), name + "-3d", pixels, width, height);  // side92: as shown
    }
  }
  ngl_vk_set_meshes(draws.data(), static_cast<uint32_t>(draws.size()));
  ngl_vk_set_sun_shadow(shadow.ok ? shadow.light : nullptr, shadow.ok ? shadow.camera : nullptr);
  SetGrass(grass);
  DepthOfField(base, frame, pictures);  // side125
  // side120: a frame without quads still draws (ngl_vk refuses a null list; side119's first live frame, the menu's,
  // had none and turned the native side off)
  const ngl_vk_vertex no_quads{};
  // side123: a movie playing: its latest frame over the whole screen, under the quads (texture 0: the picture given)
  static std::vector<uint8_t> movie;
  uint32_t movie_width = 0, movie_height = 0;
  const bool movie_shown = MovieFrameNow(movie, movie_width, movie_height);
  if (movie_shown) {
    const auto corner = [](float x, float y, float u, float v) {
      ngl_vk_vertex c{};
      c.x = x;
      c.y = y;
      c.u = u;
      c.v = v;
      c.color = 0xFFFFFFFFu;
      c.textured = 1.0f;
      c.texture = 0;
      return c;
    };
    const ngl_vk_vertex screen[6] = {corner(0, 0, 0, 0), corner(kScreenWidth, 0, 1, 0),
                                     corner(0, kScreenHeight, 0, 1), corner(kScreenWidth, 0, 1, 0),
                                     corner(kScreenWidth, kScreenHeight, 1, 1), corner(0, kScreenHeight, 0, 1)};
    vertices.insert(vertices.begin(), screen, screen + 6);
    if (pictures)
      REXLOG_INFO("cod3: ngl side: frame {}: a movie frame ({}x{}) under the quads", frame, movie_width,
                  movie_height);
  }
  if (ngl_vk_draw_quads(vertices.empty() ? &no_quads : vertices.data(), static_cast<uint32_t>(vertices.size()),
                        kScreenWidth, kScreenHeight, movie_shown ? movie.data() : nullptr, movie_width,
                        movie_height, movie_width * 4)) {
    REXLOG_WARN("cod3: ngl side: drawing frame {} failed; no more", frame);
    g_state = -1;
    return;
  }
  lap(g_parts.drawing);
  g_quiet = false;
  uint32_t width = 0, height = 0;
  const uint8_t* pixels = ngl_vk_target_pixels(&width, &height);  // nullptr when not copied out (side210)
  if (g_native.load(std::memory_order_relaxed)) {  // side120: in the window, before the game's own screenshot below
    if (g_shared_device)
      ShowOnGpu(base);  // side210
    else if (pixels)
      Show(base, pixels, width, height);
  } else if (pixels && cod3::compare::Wanted()) {
    // [rexglue-native] 2026-11-09 rn33: beside the emulated GPU, to the window's live compare (render/native_compare.h),
    // through the game's gamma table as Show does it
    cod3::compare::Hand(pixels, width, height, ShownColorsNow(base).table);
  }
  lap(g_parts.hand_over);
  if (g_live_shot && pixels) {  // rn42j: cod3_debug_ngl_side_live_shots (debug only)
    SaveShownPpm(base, ScreenshotFolder(), "ngl-side-live-" + std::to_string(frame) + "-vk", pixels, width, height);
    REXLOG_INFO("cod3: ngl side: rn42j live frame {} saved ({}x{})", frame, width, height);
  }
  if (!pictures || !pixels) return;
  const std::filesystem::path path = ScreenshotFolder() / (name + "-vk.ppm");
  SaveShownPpm(base, ScreenshotFolder(), name + "-vk", pixels, width, height);  // side92: as shown
  // [rexglue-native] 2026-11-10 rn42y: the game's picture named for its frame (report-<date>-<time>-<frame>, a name
  // cod3::shot::Request keeps as given). Named by the time alone, two pictures asked for in the same second could get
  // one name: in rn42r's bursts frames 150/151 and 1350/1351 wrote one file twice, as the screenshot thread takes a
  // job off its queue before its file is written. (The picture is still taken when that thread gets to it.)
  char report[48];
  {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[24];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    std::snprintf(report, sizeof report, "report-%s-%llu", stamp, static_cast<unsigned long long>(frame));
  }
  const std::string shot = cod3::shot::Request(cod3::shot::Kind::kReport, "ngl side", report);
  REXLOG_INFO("cod3: ngl side: frame {}: {} quads and glyphs drawn ({} textured) to {}; the game's own: {}", frame,
              quads.size(), textured, path.string(), shot.empty() ? std::string("not taken") : shot);
#else
  (void)name;
  if (g_dumps == 1) REXLOG_INFO("cod3: ngl side: this build has no ngl_vk (Vulkan or glslc missing): counts only");
#endif
}

// The grass (from 2026-11-05 15:40). GrassRenderNode__vfunc_2 (0x823F79B0) runs 0x823F6DF0, which for each patch sets
// its samplers and origin and calls 0x823F1C70: one Direct3D draw of a triangle strip whose count comes from the
// patch and its distance, returning to kGrassDrawReturn. The blades are made by the vertex shader, so for ngl_vk to
// draw them the draws from the kGrassDrawProbed-th on (kGrassDrawsLogged of them) log the device's constants, and the
// first of them also its fetch constants and saves, in the screenshot folder, both shaders and every vertex buffer it
// fetches from (ngl-side-grass-*.bin, as in guest memory). A run with dump_shaders set has the SDK's disassembly of
// the same microcode to match them against.
//
// A shader object (CreateVertexShader 0x822E68D8; the load at 0x822FB4B0, which sends them with IM_LOAD): a header
// (the vertex shader's at +0x368, the pixel shader's at +0x28) with +0x00 flags, +0x04 its own size, +0x08 the
// microcode block's size and +0x18 (+0x20 for the vertex shader's second variant, flag 0x20) the offset, from the
// header, of {microcode offset in the block, microcode size in bytes}; the block's address is at +0x1C (vertex) or
// +0x18 (pixel).
#if defined(COD3_MP)
constexpr uint32_t kGrassDrawReturn = 0x8240B794;  // the same call in 0x8240B6E8 (cod3mp's 0x823F1C70), +0xAC
#else
constexpr uint32_t kGrassDrawReturn = 0x823F1D1C;
#endif
constexpr int kGrassDrawProbed = 300;
constexpr int kGrassDrawsLogged = 4;
constexpr uint32_t kDeviceFetchConstants = 0x0400;  // 32 of 24 bytes (six words: one texture or three vertex)
constexpr int kFetchConstants = 32;
constexpr uint32_t kFetchConstantBytes = 24;
constexpr uint32_t kDevicePixelShader = 0x3080;
constexpr uint32_t kDeviceVertexShader = 0x3084;
constexpr uint32_t kDeviceIndexBuffer = 0x2F84;
constexpr uint32_t kShaderFlags = 0x00, kShaderHeaderBytes = 0x04, kShaderBlockBytes = 0x08;
constexpr uint32_t kShaderMicrocode = 0x18;  // + 8 × variant
constexpr uint32_t kShaderSecondVariant = 0x20;
constexpr uint32_t kMaxShaderBytes = 0x10000;
constexpr uint32_t kMaxVertexBufferBytes = 0x100000;

std::atomic<int> g_grass_draws{0};

// Saves bytes of guest memory at address, as they are, to name in the screenshot folder.
void SaveGuestBytes(uint8_t* base, uint32_t address, uint32_t bytes, const std::string& name) {
#if defined(COD3_NGL_VK)
  const std::filesystem::path path = ScreenshotFolder() / name;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(base + address), bytes);
  REXLOG_INFO("cod3: ngl side: grass: saved {} bytes at {:08X} to {}", bytes, address, path.string());
#else
  (void)base;
  REXLOG_INFO("cod3: ngl side: grass: {} not saved ({} bytes at {:08X}): this build has no ngl_vk", name, bytes,
              address);
#endif
}

// prefix names the saved files (side93: the grass's, and the mesh sections' for the haze): <prefix>-vs/-ps-*.bin.
void SaveShader(uint8_t* base, uint32_t shader, uint32_t header_at, uint32_t block_at, bool vertex,
                const std::string& prefix) {
  const char* kind = vertex ? "vertex" : "pixel";
  if (!DeviceReadable(shader)) {
    REXLOG_INFO("cod3: ngl side: {}: no {} shader ({:08X})", prefix, kind, shader);
    return;
  }
  const uint32_t header = shader + header_at;
  const uint32_t flags = g::ReadU32(base, header + kShaderFlags);
  const uint32_t header_bytes = g::ReadU32(base, header + kShaderHeaderBytes);
  const uint32_t block = g::ReadU32(base, shader + block_at);
  const uint32_t block_bytes = g::ReadU32(base, header + kShaderBlockBytes);
  std::string variants;
  char item[64];
  const int count = vertex && (flags & kShaderSecondVariant) ? 2 : 1;
  for (int v = 0; v < count; ++v) {
    const uint32_t entry = header + g::ReadU32(base, header + kShaderMicrocode + 8 * static_cast<uint32_t>(v));
    std::snprintf(item, sizeof item, "%smicrocode +%X (%u bytes)", v ? ", " : "", g::ReadU32(base, entry),
                  g::ReadU32(base, entry + 4));
    variants += item;
  }
  REXLOG_INFO("cod3: ngl side: {}: {} shader {:08X}: flags {:08X}, header {} bytes, block {:08X} ({} bytes), {}; "
              "object: {}",
              prefix, kind, shader, flags, header_bytes, block, block_bytes, variants, Words(base, shader, 0x10));
  const std::string name = prefix + (vertex ? "-vs" : "-ps");
  if (header_bytes > 0 && header_bytes <= kMaxShaderBytes) {
    SaveGuestBytes(base, header, header_bytes, name + "-header.bin");
  }
  if (Physical(block) && block_bytes > 0 && block_bytes <= kMaxShaderBytes) {
    SaveGuestBytes(base, block, block_bytes, name + "-block.bin");
  }
}

// The fetch constants in use, and the vertex buffers they point at (type 3: the address in dwords in bits 2-31 of the
// first word, the size in dwords in bits 2-25 of the second).
void SaveFetches(uint8_t* base, uint32_t device) {
  int saved = 0;
  for (int f = 0; f < kFetchConstants; ++f) {
    const uint32_t at = device + kDeviceFetchConstants + kFetchConstantBytes * static_cast<uint32_t>(f);
    uint32_t any = 0;
    for (uint32_t w = 0; w < kFetchConstantBytes; w += 4) any |= g::ReadU32(base, at + w);
    if (!any) continue;
    REXLOG_INFO("cod3: ngl side: grass: fetch constant {}: {}", f, Words(base, at, kFetchConstantBytes / 4));
    if ((g::ReadU32(base, at) & 3) == 2) continue;  // a texture's six words
    for (uint32_t v = 0; v < 3; ++v) {
      const uint32_t word0 = g::ReadU32(base, at + 8 * v), word1 = g::ReadU32(base, at + 8 * v + 4);
      const uint32_t bytes = ((word1 >> 2) & 0xFFFFFF) * 4;
      if ((word0 & 3) != 3 || bytes == 0 || bytes > kMaxVertexBufferBytes || saved >= 4) continue;
      const uint32_t address = GuestAddress(word0 & 0x1FFFFFFC);
      SaveGuestBytes(base, address, bytes, "ngl-side-grass-vb" + std::to_string(3 * f + static_cast<int>(v)) + ".bin");
      ++saved;
    }
  }
}

// The index buffer (Direct3D's indexed draw, 0x822F3A28, reads it from the device): flags (bit 31: 32-bit indices),
// its data's address at +0x18 and size in bytes at +0x1C.
void SaveIndices(uint8_t* base, uint32_t device) {
  const uint32_t buffer = g::ReadU32(base, device + kDeviceIndexBuffer);
  if (!DeviceReadable(buffer)) {
    REXLOG_INFO("cod3: ngl side: grass: no index buffer ({:08X})", buffer);
    return;
  }
  const uint32_t address = GuestAddress(g::ReadU32(base, buffer + 0x18)), bytes = g::ReadU32(base, buffer + 0x1C);
  REXLOG_INFO("cod3: ngl side: grass: index buffer {:08X}: {}", buffer, Words(base, buffer, 8));
  if (Physical(address) && bytes > 0 && bytes <= kMaxVertexBufferBytes) {
    SaveGuestBytes(base, address, bytes, "ngl-side-grass-ib.bin");
  }
}

void ProbeGrass(uint8_t* base, int index) {
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  REXLOG_INFO("cod3: ngl side: grass draw {} (device {:08X}): vertex constants:{}", index, device,
              ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  REXLOG_INFO("cod3: ngl side: grass draw {}:   pixel constants:{}", index,
              ConstantsText(base, device + kDevicePixelConstants, nullptr));
  // the registers the grass shader reads (c12..c15 the view-projection, c16..c51 NGL's, c80..c85 the grass's own),
  // to set beside the native path's ("grass inputs")
  std::vector<float> c(96 * 4);
  for (size_t i = 0; i < c.size(); ++i) c[i] = g::ReadF32(base, device + kDeviceVertexConstants + 4 * i);
  const uint32_t scene = g::ReadU32(base, kCurrentScene);
  REXLOG_INFO("cod3: ngl side: grass draw {}: scene {:08X} time {};{}", index, scene,
              Pointer(scene) ? g::ReadF32(base, scene + 0x420) : 0.0f,
              RegistersText(c.data(), 12, 4) + RegistersText(c.data(), 80, 6));
  if (index != kGrassDrawProbed) return;
  REXLOG_INFO("cod3: ngl side: grass draw {}:{}", index, RegistersText(c.data(), 16, 36));
  SaveShader(base, g::ReadU32(base, device + kDeviceVertexShader), 0x368, 0x1C, true, "ngl-side-grass");
  SaveShader(base, g::ReadU32(base, device + kDevicePixelShader), 0x28, 0x18, false, "ngl-side-grass");
  SaveFetches(base, device);
  SaveIndices(base, device);
}

// side84: the old material classes' draws. nglDrawMeshSection (0x8213C9F8; COD3-NGL-MAP.md: cdSky's five draws a
// frame in the forest, cdDynamicDecal's one) draws through Direct3D itself, so the native side never meets those meshes
// in the scene lists, and the sky is missing from its picture. A draw whose return address is in its body is logged
// with what the GPU is handed (the fetch constants in use and the first words of each vertex buffer, the index buffer,
// the vertex constants: the sky's matrix); side85 draws the sky natively from them (KeepSkyDraw, SkyDraws).
#if defined(COD3_MP)
// cod3mp's nglDrawMeshSection (cod3_relatives.tsv), its 204 bytes as the campaign's
constexpr uint32_t kMeshSectionStart = 0x82155A80, kMeshSectionEnd = 0x82155A80 + 0xCC;
#else
constexpr uint32_t kMeshSectionStart = 0x8213C9F8, kMeshSectionEnd = 0x8213CC00;  // the end: a guess past its draws
#endif
constexpr int kMeshSectionProbed = 3000;  // about frame 600, in play
constexpr int kMeshSectionsLogged = 8;
constexpr size_t kSectionPairsSaved = 16;  // side180: shader pairs saved (was 4)
std::atomic<int> g_mesh_section_draws{0};

void ProbeMeshSection(uint8_t* base, uint32_t call_site, int index) {
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  REXLOG_INFO("cod3: ngl side: mesh section draw {} from {:08X}: vertex constants:{}", index, call_site,
              ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  for (int f = 0; f < kFetchConstants; ++f) {
    const uint32_t at = device + kDeviceFetchConstants + kFetchConstantBytes * static_cast<uint32_t>(f);
    uint32_t any = 0;
    for (uint32_t w = 0; w < kFetchConstantBytes; w += 4) any |= g::ReadU32(base, at + w);
    if (!any) continue;
    std::string buffers;
    if ((g::ReadU32(base, at) & 3) != 2) {  // vertex fetches (type 3), three to a constant: each buffer's first words
      for (uint32_t v = 0; v < 3; ++v) {
        const uint32_t word0 = g::ReadU32(base, at + 8 * v);
        const uint32_t address = GuestAddress(word0 & 0x1FFFFFFC);
        if ((word0 & 3) == 3 && Physical(address)) {
          buffers += " | buffer " + std::to_string(3 * f + static_cast<int>(v)) + ": " + Words(base, address, 12);
        }
      }
    }
    REXLOG_INFO("cod3: ngl side: mesh section draw {}: fetch constant {}: {}{}", index, f,
                Words(base, at, kFetchConstantBytes / 4), buffers);
  }
  const uint32_t buffer = g::ReadU32(base, device + kDeviceIndexBuffer);
  REXLOG_INFO("cod3: ngl side: mesh section draw {}: index buffer {:08X}: {}", index, buffer,
              DeviceReadable(buffer) ? Words(base, buffer, 8) : std::string("not readable"));
  // side93: the haze. A world surface's draw hands the fog as vertex constants (c12: the amount before the start, the
  // start, 1 / (end - start), the amount's rise; c13: the eye in the object's space; c16: the color), linear by the
  // distance from the eye as ngl_vk does, yet the game's distance looks hazier. Each new pair of shaders is saved
  // (ngl-side-section<N>-vs/-ps-*.bin) with the pixel constants, to read what the pixel shader does with it.
  static std::mutex saved_mutex;
  static std::unordered_set<uint64_t> saved;
  const uint32_t vertex_shader = g::ReadU32(base, device + kDeviceVertexShader);
  const uint32_t pixel_shader = g::ReadU32(base, device + kDevicePixelShader);
  {
    std::lock_guard lock(saved_mutex);
    if (saved.size() >= kSectionPairsSaved) return;
    if (!saved.insert(uint64_t{vertex_shader} << 32 | pixel_shader).second) return;
  }
  // side180: with the vertex constants too (the world's nearby lights, LIGHT_POSITION1/2 and LIGHT_COLOR1/2, are
  // vertex constants), for more pairs
  REXLOG_INFO("cod3: ngl side: mesh section draw {}: vertex constants:{}", index,
              ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  REXLOG_INFO("cod3: ngl side: mesh section draw {}: pixel constants:{}", index,
              ConstantsText(base, device + kDevicePixelConstants, nullptr));
  const std::string prefix = "ngl-side-section" + std::to_string(index);
  SaveShader(base, vertex_shader, 0x368, 0x1C, true, prefix);
  SaveShader(base, pixel_shader, 0x28, 0x18, false, prefix);
}

// side85: the sky, drawn natively from what nglDrawMeshSection hands Direct3D. side84 found, at the sky's draws: in
// texture fetch constant 0 the layer's picture (a DXT5 256 x 256 and a 128 x 128 in the forest); in vertex fetch
// constant 95 the dome (65 corners of six words: position x, y, z, u, v and a D3DCOLOR); the index buffer (a triangle
// list, 336 16-bit indices); and in c0..c3 the camera's view-projection without its position (Direct3D's columns;
// c12..c15 are the same with it), so the dome stays centered on the eye. Each such draw is kept (KeepSkyDraw), and the
// next native picture draws the ones from the last list sends first: at the far plane (z = w, as Big Red One's
// ngl_present.c does for its sky), the first layer opaque and the others blended over it by their alpha, in the order
// the game drew them. A draw whose c0..c3 has a position (cdDynamicDecal's, the other user of nglDrawMeshSection) is
// left out. The layers' blend states (side98) and drift (side102) are the draws' own.
struct SkyDraw {
  uint32_t texture[6] = {};  // the texture fetch constant
  uint32_t vertices = 0, vertex_words = 0, vertex_endian = 0;
  uint32_t indices = 0, index_bytes = 0;
  float columns[16] = {};  // c0..c3
  uint64_t list = 0;       // the list send it was last seen after
  // side98: how the layer goes onto the ones under it, RB_BLENDCONTROL0 as the draw sends it (bits 0-4 the source
  // factor, 5-7 the operation, 8-12 the destination factor), and the pixel shader's first two constants (a tint?)
  uint32_t blend_control = 0;
  float pixel[8] = {};
  // side102: the layer's drift, c4's x and y, which its vertex shader (shader_24CFBA2A030227A9) adds to the texture
  // coordinates (the game moves it with the clock: the clouds' scroll)
  float drift[2] = {};
};
constexpr uint32_t kDeviceBlendControl0 = 0x28B8;  // register 0x2201 (+0x28B4 holds 0x2200-0x220B)
constexpr uint32_t kSkyVertexFetch = 95;  // the stream the game's shaders read (fetch constant 31's third)
constexpr uint32_t kSkyVertexWords = 6;
constexpr uint32_t kSkyMaxCorners = 4096;
constexpr uint32_t kSkyMaxIndexBytes = 0x20000;
constexpr size_t kSkyMaxDraws = 16;
constexpr uint64_t kSkyRecent = 3;  // list sends a sky draw is still drawn for
std::atomic<uint64_t> g_list_sends{0};
std::mutex g_sky_mutex;
std::vector<SkyDraw> g_sky;  // in the order the game first drew them

// side191: the place of each sky draw among its list's sky draws (for cod3_debug_skip_game_sky_layers), and whether
// this thread's draw is to be left out of the game's picture (taken by SkipGameDraw)
thread_local bool t_skip_game_draw = false;
std::mutex g_sky_order_mutex;
uint64_t g_sky_order_list = ~uint64_t{0};
uint32_t g_sky_order_next = 0;

void KeepSkyDraw(uint8_t* base) {
  static std::atomic<int> shown{0}, refused{0};
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  SkyDraw s;
  for (uint32_t i = 0; i < 6; ++i) s.texture[i] = g::ReadU32(base, device + kDeviceFetchConstants + 4 * i);
  const uint32_t vertex_at = device + kDeviceFetchConstants + 8 * kSkyVertexFetch;
  const uint32_t word0 = g::ReadU32(base, vertex_at), word1 = g::ReadU32(base, vertex_at + 4);
  s.vertices = GuestAddress(word0 & 0x1FFFFFFC);
  s.vertex_words = (word1 >> 2) & 0xFFFFFF;
  s.vertex_endian = word1 & 3;
  const uint32_t buffer = g::ReadU32(base, device + kDeviceIndexBuffer);
  if (DeviceReadable(buffer)) {
    s.indices = GuestAddress(g::ReadU32(base, buffer + kBufferFetch));
    s.index_bytes = g::ReadU32(base, buffer + kBufferFetch + 4);
  }
  for (uint32_t i = 0; i < 16; ++i) s.columns[i] = g::ReadF32(base, device + kDeviceVertexConstants + 4 * i);
  for (uint32_t i = 0; i < 2; ++i) {
    s.drift[i] = g::ReadF32(base, device + kDeviceVertexConstants + 4 * (16 + i));
    if (!std::isfinite(s.drift[i])) s.drift[i] = 0.0f;
  }
  s.blend_control = g::ReadU32(base, device + kDeviceBlendControl0);
  for (uint32_t i = 0; i < 8; ++i) s.pixel[i] = g::ReadF32(base, device + kDevicePixelConstants + 4 * i);
  const float position = std::fabs(s.columns[3]) + std::fabs(s.columns[7]) + std::fabs(s.columns[15]);
  const char* why = nullptr;
  bool positioned_view = false;  // side190
  if ((s.texture[0] & 3) != 2) {
    why = "no texture in fetch constant 0";
  } else if ((word0 & 3) != 3 || !Physical(s.vertices) || !s.vertex_words || s.vertex_words % kSkyVertexWords ||
             s.vertex_words / kSkyVertexWords > kSkyMaxCorners) {
    why = "no dome in vertex fetch constant 95";
  } else if (!Physical(s.indices) || s.index_bytes < 6 || s.index_bytes > kSkyMaxIndexBytes) {
    why = "no index buffer";
  } else if (!(position < 1.0f)) {
    why = "its view-projection has a position (not the sky)";
    positioned_view = true;
  } else if ((s.texture[1] & 0x3F) == 22 || (s.texture[1] & 0x3F) == 23) {
    // side148: mayenne's draws of the depth (k_24_8, k_24_8_FLOAT; 52-corner strips, not domes) filled the 16 kept
    // draws and pushed its 5 sky layers out, leaving the sky black
    why = "it reads the depth (not the sky)";
  }
  if (why) {
    if (refused.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_INFO("cod3: ngl side: mesh section draw left out of the sky: {}", why);
    }
    // side190: the mesh section draws with a positioned view-projection (left out of the sky, and drawn by nothing
    // else natively): Island's 82001CE4 nodes (Render 0x820D4548) draw mesh sections, maybe the rain, and Black Burn
    // and Saint-Lo have 2-3 such draws a frame too. What they are, once in play: texture, blend, corners, the
    // view-projection's position, and their shaders (ngl-side-positioned<N>, up to 4 pixel shaders).
    static std::atomic<int> positioned{0};
    static std::mutex positioned_mutex;
    static std::vector<uint32_t> positioned_shaders;
    if (positioned_view && REXCVAR_GET(cod3_debug_ngl_side) && g_frames >= 1700) {
      if (positioned.fetch_add(1, std::memory_order_relaxed) < 12) {
        REXLOG_INFO("cod3: ngl side: positioned section draw (frame {}): texture {}x{} format {} at {:08X}, blend "
                    "{:08X}, {} corners at {:08X} (byte order {}), {} index bytes; view-projection row 3 ({:.3g} "
                    "{:.3g} {:.3g} {:.3g}); pixel c0 ({:.3f} {:.3f} {:.3f} {:.3f}) c1 ({:.3f} {:.3f} {:.3f} {:.3f})",
                    g_frames, (s.texture[2] & 0x1FFF) + 1, ((s.texture[2] >> 13) & 0x1FFF) + 1, s.texture[1] & 0x3F,
                    s.texture[1] & 0x1FFFF000, s.blend_control, s.vertex_words / kSkyVertexWords, s.vertices,
                    s.vertex_endian, s.index_bytes, s.columns[3], s.columns[7], s.columns[11], s.columns[15],
                    s.pixel[0], s.pixel[1], s.pixel[2], s.pixel[3], s.pixel[4], s.pixel[5], s.pixel[6], s.pixel[7]);
      }
      const uint32_t pixel_shader = g::ReadU32(base, device + kDevicePixelShader);
      int save_as = -1;
      {
        std::lock_guard<std::mutex> lock(positioned_mutex);
        if (positioned_shaders.size() < 4 &&
            std::find(positioned_shaders.begin(), positioned_shaders.end(), pixel_shader) ==
                positioned_shaders.end()) {
          save_as = static_cast<int>(positioned_shaders.size());
          positioned_shaders.push_back(pixel_shader);
        }
      }
      if (save_as >= 0) {
        const std::string prefix = "ngl-side-positioned" + std::to_string(save_as);
        REXLOG_INFO("cod3: ngl side: {}: vertex constants:{}", prefix,
                    ConstantsText(base, device + kDeviceVertexConstants, nullptr));
        SaveShader(base, g::ReadU32(base, device + kDeviceVertexShader), 0x368, 0x1C, true, prefix);
        SaveShader(base, pixel_shader, 0x28, 0x18, false, prefix);
      }
    }
    return;
  }
  s.list = g_list_sends.load(std::memory_order_relaxed);
  if (const uint32_t skipped = static_cast<uint32_t>(REXCVAR_GET(cod3_debug_skip_game_sky_layers))) {  // side191
    uint32_t place = 0;
    {
      std::lock_guard<std::mutex> lock(g_sky_order_mutex);
      if (g_sky_order_list != s.list) {
        g_sky_order_list = s.list;
        g_sky_order_next = 0;
      }
      place = g_sky_order_next++;
    }
    t_skip_game_draw = place < 32 && ((skipped >> place) & 1u) != 0;
  }
  {  // side187: every sky draw of a few lists once in play, before they are merged by dome and texture below (whether
     // the game draws a layer twice a frame, which the merge would keep once)
    static std::atomic<int> listed{0};
    if (g_frames >= 1700 && listed.fetch_add(1, std::memory_order_relaxed) < 32) {
      REXLOG_INFO("cod3: ngl side: sky draw of list {} (frame {}): corners {:08X} x{}, indices {:08X}, texture "
                  "{:08X} format {}, blend {:08X}, uv drift {:.4f} {:.4f}",
                  s.list, g_frames, s.vertices, s.vertex_words / kSkyVertexWords, s.indices,
                  s.texture[1] & 0x1FFFF000, s.texture[1] & 0x3F, s.blend_control, s.drift[0], s.drift[1]);
    }
  }
  std::lock_guard<std::mutex> lock(g_sky_mutex);
  for (SkyDraw& kept : g_sky) {
    if (kept.vertices == s.vertices && kept.indices == s.indices && kept.texture[1] == s.texture[1]) {
      kept = s;
      return;
    }
  }
  if (g_sky.size() >= kSkyMaxDraws) {  // room for a new one: the one seen longest ago goes
    g_sky.erase(std::min_element(g_sky.begin(), g_sky.end(),
                                 [](const SkyDraw& a, const SkyDraw& b) { return a.list < b.list; }));
  }
  g_sky.push_back(s);
  if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO("cod3: ngl side: sky layer {}: texture {}x{} format {} at {:08X} (fetch signs {:02X}, exponent "
                "field {}); {} corners at {:08X} (byte order {}); {} index bytes at {:08X}; near {:.3f}; blend {:08X}; "
                "pixel c0 ({:.3f} {:.3f} {:.3f} {:.3f}) c1 ({:.3f} {:.3f} {:.3f} {:.3f})",
                g_sky.size(), (s.texture[2] & 0x1FFF) + 1, ((s.texture[2] >> 13) & 0x1FFF) + 1, s.texture[1] & 0x3F,
                s.texture[1] & 0x1FFFF000, (s.texture[0] >> 2) & 0xFF, (s.texture[3] >> 13) & 0x3F,
                s.vertex_words / kSkyVertexWords, s.vertices, s.vertex_endian, s.index_bytes, s.indices,
                s.columns[11], s.blend_control, s.pixel[0], s.pixel[1], s.pixel[2], s.pixel[3], s.pixel[4],
                s.pixel[5], s.pixel[6], s.pixel[7]);
    // side171: what the layer brings: its first, middle and last corners' colors (word 5, 0xAARRGGBB as stored) and
    // the first 16 bytes of its texture (two blocks of a DXT texture), to tell a black layer from a blank one
    const uint32_t corners = s.vertex_words / kSkyVertexWords;
    const uint32_t texture = 0xA0000000u | (s.texture[1] & 0x1FFFF000u);
    if (corners > 0) {
      auto color = [&](uint32_t i) { return GpuU32(base, s.vertices + 4 * kSkyVertexWords * i + 20); };
      REXLOG_INFO("cod3: ngl side: sky layer {}: corner colors {:08X} {:08X} {:08X} (byte order {}); texture "
                  "starts {:08X} {:08X} {:08X} {:08X}",
                  g_sky.size(), color(0), color(corners / 2), color(corners - 1), s.vertex_endian,
                  GpuU32(base, texture), GpuU32(base, texture + 4), GpuU32(base, texture + 8),
                  GpuU32(base, texture + 12));
    }
    // side177: the layer's shaders (ngl-side-sky<N>-ps/-vs-header.bin and -block.bin), for tools/ps_names.py to name
    // against a dump_shaders folder: what the game's pixel shader does with the layer's texture and corner colors
    const std::string prefix = "ngl-side-sky" + std::to_string(g_sky.size());
    SaveShader(base, g::ReadU32(base, device + kDevicePixelShader), 0x28, 0x18, false, prefix);
    SaveShader(base, g::ReadU32(base, device + kDeviceVertexShader), 0x368, 0x1C, true, prefix);
  }
}

// side104: the effects' quads (smoke, dust, sparks, muzzle flashes), drawn natively. The game's effects go through one
// quad batcher (0x820FAED0; its vertex shader shader_AACFBC52742E4628): four corners of 12 words a quad (a position
// in the world, a D3DCOLOR at word 3, the texture coordinates at words 7-8), six indices a quad, flushed with
// DrawIndexedVertices from 0x820FAF80. On the frame before a dump KeepEffectDraw copies each flush's quads (the
// batcher's quad count at 0x8290B5AC, its first index at 0x8290B5B4; the corners from vertex fetch constant 95, as the
// sky's), the texture in fetch constant 0, the blend state and the view-projection (c0..c3). The native picture draws
// them blended after the world, in their corners' colors and the scene's fog. side111 adds the tint in c16 and the
// near fade (c14); side112 the soft edge where a quad meets the scene (its pixel shader, shader_8356D96B00DE9DED,
// fades alpha by the depth difference; ngl_vk's soft_depth). side110: cod3mp's batcher is 0x8210CDF8
// (cod3_relatives.tsv), the same 256 bytes with its globals moved: the quad count at 0x82A8843C, the first index at
// 0x82A88444, the flush returning at 0x8210CEA8 (the same +0xB0).
#if defined(COD3_NGL_VK)  // the effects' quads are kept only for ngl_vk to draw
#if defined(COD3_MP)
constexpr uint32_t kEffectDrawReturn = 0x8210CEA8;
constexpr uint32_t kEffectQuadCount = 0x82A8843C;
constexpr uint32_t kEffectFirstIndex = 0x82A88444;
#else
constexpr uint32_t kEffectDrawReturn = 0x820FAF80;
constexpr uint32_t kEffectQuadCount = 0x8290B5AC;
constexpr uint32_t kEffectFirstIndex = 0x8290B5B4;
#endif
#endif
// side107: what the game's lighting (0x82142360, called with 0x829112A0) hands a model's shaders. At 0x829112A0: the
// main light's color (+0x00) and way (+0x10; with a kind 3 light, the sun's way and the first kind 1's color), the
// nearest point light (+0x30 color, +0x40 place, +0x50 1 / radius) and the next (+0x60, +0x70, +0x80); at 0x82911390
// nine four-float rows, a quadratic form in the normal (xx, yy, zz, then the cross terms, then x, y, z), the set's
// ambient added into xx, yy and zz. The models' draw returns at 0x82144364.
// side194: cod3mp's: the same computed call inside 0x8215BD78 (the twin of 0x82143F70 in cod3_relatives.tsv; its
// computed calls sit 0x60-0x64 earlier than the campaign's, the second one, to 0x82158E58 = 0x821410A0's twin, at
// 0x8215C104); the block 0x82A8E1B0 (mp/globals_sp_to_mp.tsv, 6 of 6 uses) and its rows at 0x82A8E2A0, written as
// the campaign's 0x82911390 (mp/decomp/82150000.c)
#if defined(COD3_MP)
constexpr uint32_t kModelDrawReturn = 0x8215C108;
constexpr uint32_t kModelLights = 0x82A8E1B0;
constexpr uint32_t kModelLightRows = 0x82A8E2A0;
#else
constexpr uint32_t kModelDrawReturn = 0x82144364;
constexpr uint32_t kModelLights = 0x829112A0;
constexpr uint32_t kModelLightRows = 0x82911390;
#endif
#if defined(COD3_NGL_VK)  // (the effects' quads, kept for EffectDraws)
constexpr uint32_t kEffectCornerWords = 12;
constexpr uint32_t kEffectMaxQuads = 4096;
// [rexglue-native] 2026-11-10 rn42n: g_effects holds the flushes of two lists (the one drawn and the one being made),
// and was capped at 256 in all. St-Bert's busy moments flush 120-240 a list, so the list being made lost what didn't
// fit, and the next frame drew only the rest: 138 then 118, 172 then 84, 237 then 19 on alternate frames (rn42l,
// rn42m: every pair adds up to 256). That was the native smoke and dust flickering "really fast" (owner 09:12).
constexpr size_t kEffectMaxDraws = 2048;

struct EffectDraw {
  uint32_t texture[6] = {};  // the texture fetch constant
  uint32_t blend_control = 0;
  float columns[16] = {};    // c0..c3
  float pixel_c0[4] = {};    // side114: the pixel shader's c0: color x c0.w + c0.xyz (its fog)
  bool reversed = false;     // side115: c0..c3's depth runs backward (near 1, far 0), as the scenes' cameras do
  bool soft = false;         // rn10: drawn with the soft pair of shaders (ShaderNames), faded near what's behind it
  uint32_t quads = 0;
  uint64_t list = 0;
  std::vector<float> corners;  // NGL_VK_CORNER_FLOATS a corner, four a quad
};
std::mutex g_effect_mutex;
std::vector<EffectDraw> g_effects;
uint32_t g_effect_pattern[6] = {0, 1, 2, 0, 2, 3};  // a quad's six indices, as the game's index buffer has them
std::atomic<bool> g_effect_pattern_read{false};

// [rexglue-native] 2026-11-09 rn10: whether a shader's constant table (in its header, where tools/ps_names.py reads
// the names) names `name`. The batcher draws with two pairs of shaders (rn8, rn9's shader dump of The Mace): a plain
// one (vertex shader shader_6AB7A24B1D031539: LOCAL_TO_SCREEN, ALPHAFADE, BLENDCOLOR, UVSCALE, NAN_VALUE; pixel shader
// C9AB02172C3C1C0F: texture x color, its alpha too) and a soft one (vertex shader AACFBC52742E4628 adds LOCAL_TO_VIEW,
// VIEW_TO_SCREEN and Z_FEATHER_RANGE; pixel shader 8356D96B00DE9DED fades the alpha by the scene's depth, read through
// DepthSampler). Only the soft one fades; with the plain one c4..c12 are whatever the last soft flush left there.
bool ShaderNames(uint8_t* base, uint32_t shader, uint32_t header_at, const char* name) {
  if (!DeviceReadable(shader)) return false;
  const uint32_t header = shader + header_at;
  const uint32_t bytes = g::ReadU32(base, header + kShaderHeaderBytes);
  if (bytes == 0 || bytes > kMaxShaderBytes) return false;
  const uint8_t* from = base + header;
  const uint8_t* to = from + bytes;
  return std::search(from, to, name, name + std::strlen(name)) != to;
}

void KeepEffectDraw(uint8_t* base) {
  static std::atomic<int> refused{0}, shown{0};
  if (!Live() && g_frames % DumpEvery() != DumpEvery() - 1) return;  // the frame a dump draws
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  const uint32_t quads = g::ReadU32(base, kEffectQuadCount) & 0x7FFFFFFF;
  const uint32_t first_index = g::ReadU32(base, kEffectFirstIndex) & 0x7FFFFFFF;
  const uint32_t first_corner = first_index / 6 * 4;
  const uint32_t vertex_at = device + kDeviceFetchConstants + 8 * kSkyVertexFetch;
  const uint32_t word0 = g::ReadU32(base, vertex_at), word1 = g::ReadU32(base, vertex_at + 4);
  const uint32_t vertices = GuestAddress(word0 & 0x1FFFFFFC);
  const uint32_t words = (word1 >> 2) & 0xFFFFFF, endian = word1 & 3;
  const char* why = nullptr;
  if (!quads || quads > kEffectMaxQuads) {
    why = "no quads, or too many";
  } else if ((word0 & 3) != 3 || !Physical(vertices)) {
    why = "no corners in vertex fetch constant 95";
  } else if (uint64_t{first_corner + 4 * quads} * kEffectCornerWords > words) {
    why = "its quads run past the corners";
  } else if ((g::ReadU32(base, device + kDeviceFetchConstants) & 3) != 2) {
    why = "no texture in fetch constant 0";
  }
  if (why) {
    if (refused.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_INFO("cod3: ngl side: effect flush left out: {} ({} quads from index {})", why, quads, first_index);
    }
    return;
  }
  if (!g_effect_pattern_read.exchange(true)) {  // the game's corner order, from its index buffer, once
    const uint32_t buffer = g::ReadU32(base, device + kDeviceIndexBuffer);
    const uint32_t indices = DeviceReadable(buffer) ? GuestAddress(g::ReadU32(base, buffer + kBufferFetch)) : 0;
    for (int order = 0; order < 2 && Physical(indices); ++order) {
      uint32_t pattern[6];
      bool fits = true;
      for (uint32_t k = 0; k < 6 && fits; ++k) {
        const uint8_t* p = base + indices + 2 * (first_index + k);
        const uint32_t value = order == 0 ? (uint32_t{p[0]} << 8 | p[1]) : (uint32_t{p[1]} << 8 | p[0]);
        fits = value >= first_corner && value < first_corner + 4;
        pattern[k] = value - first_corner;
      }
      if (fits) {
        std::memcpy(g_effect_pattern, pattern, sizeof pattern);
        REXLOG_INFO("cod3: ngl side: an effect quad's indices: {} {} {} {} {} {}", pattern[0], pattern[1], pattern[2],
                    pattern[3], pattern[4], pattern[5]);
        break;
      }
    }
  }
  EffectDraw e;
  // rn42g: the channels the flush writes: RB_COLOR_MASK (register 0x2104) in the device's copy of 0x2100-0x2114 (+0x284C)
  const uint32_t color_mask = g::ReadU32(base, device + kDeviceColorMask) & 0xF;
  // rn10: which pair of shaders the flush is drawn with (ShaderNames): only the soft pair fades near the scene
  e.soft = ShaderNames(base, g::ReadU32(base, device + kDeviceVertexShader), 0x368, "Z_FEATHER_RANGE") &&
           ShaderNames(base, g::ReadU32(base, device + kDevicePixelShader), 0x28, "DepthSampler");
  for (uint32_t i = 0; i < 6; ++i) e.texture[i] = g::ReadU32(base, device + kDeviceFetchConstants + 4 * i);
  e.blend_control = g::ReadU32(base, device + kDeviceBlendControl0);
  for (uint32_t i = 0; i < 16; ++i) e.columns[i] = g::ReadF32(base, device + kDeviceVertexConstants + 4 * i);
  // side114: the pixel shader (8356D96B) writes texture x color x c0.w + c0.xyz, the alpha untouched: the flush's own
  // fog, set by the game for the batch, in place of the per-pixel distance fog the world takes.
  for (uint32_t i = 0; i < 4; ++i) e.pixel_c0[i] = g::ReadF32(base, device + kDevicePixelConstants + 4 * i);
  e.quads = quads;
  // side105: the vertex shader's c17.xy scales the texture coordinates (a flipbook's frame in a grid of frames: a
  // corner's coordinates count frames), c16 tints the color and c14 fades the near quads. Direct3D's copy is read
  // here and c17 used when it is a whole-number grid (1/n for x and y); the others are logged to see if they're live.
  float c14[4], c16[4], c17[4];
  // side112: the soft edge. The vertex shader takes the corner's view position (c4..c7), pushes it c12.x farther
  // along the view, and hands the pixel shader 1 / (that point's depth - the corner's), through the projection's z
  // and w rows (c10, c11); the pixel shader fades the alpha by saturate((the scene's depth - its own) x that).
  float soft[7][4];  // c4, c5, c6, c7, c10, c11, c12
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t registers[7] = {4, 5, 6, 7, 10, 11, 12};
    for (uint32_t r = 0; r < 7; ++r) {
      soft[r][i] = g::ReadF32(base, device + kDeviceVertexConstants + 16 * registers[r] + 4 * i);
    }
    c14[i] = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 14 + 4 * i);
    c16[i] = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 16 + 4 * i);
    c17[i] = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 17 + 4 * i);
  }
  const auto grid = [](float f) {
    if (!(f > 0.0f && f <= 1.0f)) return false;
    const float frames = 1.0f / f;
    return std::fabs(frames - std::round(frames)) < 1e-3f;
  };
  const bool flipbook = grid(c17[0]) && grid(c17[1]);
  // side115: c10 and c11 are the projection's z and w rows (the view looks down +z); a depth that falls with the
  // distance is the backward depth SceneCamera turns over for the world. The effects were drawn with it unturned,
  // so ngl_vk's depth test passed them in front of everything (cod3mp's mist through walls and hills).
  {
    const auto view_depth = [&soft](float z) {
      const float w = soft[5][2] * z + soft[5][3];
      return (soft[4][2] * z + soft[4][3]) / (w != 0.0f ? w : 1.0f);
    };
    const bool reversed_view = view_depth(1000.0f) < view_depth(10.0f);
    // rn10: c10 and c11 are the soft pair's alone; the plain pair's flushes find there whatever was set last. Theirs
    // is read from c0..c3 (which both pairs take): two points along the view (where c3's w is 10 and 1000, on the
    // line through c3.xyz) and their depths z / w.
    const float* z_row = &e.columns[8];
    const float* w_row = &e.columns[12];
    const float along = w_row[0] * w_row[0] + w_row[1] * w_row[1] + w_row[2] * w_row[2];
    const float z_along = z_row[0] * w_row[0] + z_row[1] * w_row[1] + z_row[2] * w_row[2];
    const auto screen_depth = [&](float w) { return ((w - w_row[3]) / along * z_along + z_row[3]) / w; };
    const bool reversed_screen =
        std::isfinite(along) && along > 0.0f && screen_depth(1000.0f) < screen_depth(10.0f);
    e.reversed = e.soft ? reversed_view : reversed_screen;
    static std::atomic<int> differ{0};
    if (e.soft && reversed_view != reversed_screen && Watching() && differ.fetch_add(1) < 8) {
      REXLOG_INFO("cod3: ngl side: effect flush (soft): c0..c3 say depth {} but c10/c11 say {}",
                  reversed_screen ? "backward" : "forward", reversed_view ? "backward" : "forward");
    }
  }
  e.corners.assign(static_cast<size_t>(4 * quads) * NGL_VK_CORNER_FLOATS, 0.0f);
  for (uint32_t q = 0; q < quads; ++q) {
    bool numbers = true;
    for (uint32_t c = 4 * q; c < 4 * q + 4; ++c) {
      const uint32_t at = vertices + 4 * kEffectCornerWords * (first_corner + c);
      float* out = &e.corners[static_cast<size_t>(c) * NGL_VK_CORNER_FLOATS];
      for (uint32_t k = 0; k < 3; ++k) out[k] = VertexFloat(base, at + 4 * k, endian);
      out[3] = VertexFloat(base, at + 28, endian) * (flipbook ? c17[0] : 1.0f);
      out[4] = out[6] = VertexFloat(base, at + 32, endian) * (flipbook ? c17[1] : 1.0f);
      out[5] = 1e8f;  // rn10: the plain pair's flushes aren't faded (EffectDraws: no soft_depth)
      if (e.soft) {  // side112: the soft edge's scale, in the lightmap u's place (ngl_vk's soft_depth)
        const auto dot = [&out](const float* row) {
          return row[0] * out[0] + row[1] * out[1] + row[2] * out[2] + row[3];
        };
        const float view[4] = {dot(soft[0]), dot(soft[1]), dot(soft[2]) + soft[6][0], dot(soft[3])};
        const auto dot4 = [&view](const float* row) {
          return row[0] * view[0] + row[1] * view[1] + row[2] * view[2] + row[3] * view[3];
        };
        const float own = dot(&e.columns[8]) / dot(&e.columns[12]);
        const float farther = dot4(soft[4]) / dot4(soft[5]);
        // side115: in the game's backward depth the point c12.x farther has the smaller depth, so the game's
        // scale is negative (side112's log: -2e4 to -1e5); turned over for ngl_vk (1 - depth) it changes sign.
        // A scale that still isn't positive (or a 1 / 0) isn't faded, rather than hidden.
        float scale = 1.0f / (farther - own);
        if (e.reversed) scale = -scale;
        out[5] = std::isfinite(scale) && scale > 0.0f ? scale : 1e8f;
      }
      uint32_t color = g::ReadU32(base, at + 12);  // a D3DCOLOR, 0xAARRGGBB
      if (endian == 0) color = __builtin_bswap32(color);
      {  // side111: as the vertex shader does, the color times c16 and the alpha faded near the eye by
         // saturate(w x c14.x + c14.y), w the corner's clip w (its distance along the view, through c3)
        const float w = e.columns[12] * out[0] + e.columns[13] * out[1] + e.columns[14] * out[2] + e.columns[15];
        float fade = w * c14[0] + c14[1];
        fade = std::isfinite(fade) ? std::min(std::max(fade, 0.0f), 1.0f) : 1.0f;
        const float scale[4] = {c16[2], c16[1], c16[0], c16[3] * fade};  // blue, green, red, alpha (low byte first)
        uint32_t tinted = 0;
        for (uint32_t k = 0; k < 4; ++k) {
          const float v = static_cast<float>((color >> (8 * k)) & 255u) * (std::isfinite(scale[k]) ? scale[k] : 1.0f);
          tinted |= static_cast<uint32_t>(std::min(std::max(v, 0.0f), 255.0f) + 0.5f) << (8 * k);
        }
        color = tinted;
      }
      std::memcpy(&out[7], &color, 4);
      for (uint32_t k = 0; k < 5; ++k) numbers = numbers && std::isfinite(out[k]);
    }
    if (!numbers) {  // a quad with a corner that isn't a number: left as a point at the origin, drawing nothing
      const auto quad = e.corners.begin() + static_cast<std::ptrdiff_t>(4 * q) * NGL_VK_CORNER_FLOATS;
      std::fill(quad, quad + 4 * NGL_VK_CORNER_FLOATS, 0.0f);
    }
  }
  e.list = g_list_sends.load(std::memory_order_relaxed);
  // [rexglue-native] 2026-11-10 rn42f: the native side's flushes beside the GPU's (LogEffectFlushes): those kept with
  // 0D84C000 or with the square flush's fade (c14.x -0.0005882), once per (texture, quad count, kind), up to 60 lines
  // from frame 600, with the frame, the first index, c14, the first corner (place, and color after c16 and the fade)
  // and NGL's own record of stage 0's texture (0x82910D68, FUN_8213EB90's cache), to see which texture the batcher
  // meant (debug only). rn42g: and the color mask, with any flush that writes no color, and the first 8 other textures'
  // flushes from frame 1800 (once per texture), to check that the mask reads F for them.
  if (REXCVAR_GET(cod3_debug_ngl_side) && g_frames >= 600 && quads) {
    const uint32_t texture_at = e.texture[1] & 0x1FFFF000;
    const bool square_kind = std::fabs(c14[0] + 0.0005882f) < 0.00002f;
    static std::mutex rn42f_lock;
    static std::vector<uint64_t> rn42f_seen;
    static std::vector<uint32_t> rn42g_others;
    bool other = false;
    if (texture_at != 0x0D84C000u && !square_kind && (color_mask & 7u) != 0 && g_frames >= 1800) {
      std::lock_guard<std::mutex> hold(rn42f_lock);
      other = rn42g_others.size() < 8 &&
              std::find(rn42g_others.begin(), rn42g_others.end(), texture_at) == rn42g_others.end();
      if (other) rn42g_others.push_back(texture_at);
    }
    if (texture_at == 0x0D84C000u || square_kind || (color_mask & 7u) == 0 || other) {
      const uint64_t key = (uint64_t{texture_at} << 32) ^ (uint64_t{quads} << 4) ^ (square_kind ? 1u : 0u) ^
                           (other ? 2u : 0u);
      std::lock_guard<std::mutex> hold(rn42f_lock);
      if (rn42f_seen.size() < 60 && std::find(rn42f_seen.begin(), rn42f_seen.end(), key) == rn42f_seen.end()) {
        rn42f_seen.push_back(key);
        const float* first = e.corners.data();
        uint32_t color;
        std::memcpy(&color, &first[7], 4);
        // rn43d: NGL's record of stage 0's texture, the campaign's address (cod3mp's isn't found: logged as 0)
#if defined(COD3_MP)
        constexpr uint32_t kStageZeroRecord = 0;
#else
        constexpr uint32_t kStageZeroRecord = 0x82910D68;
#endif
        const uint32_t stage_zero = kStageZeroRecord ? g::ReadU32(base, kStageZeroRecord) : 0u;
        REXLOG_INFO("cod3: ngl side: rn42f native flush (frame {}): texture {:08X}, {} quads from index {}, {}, c14 "
                    "({:.4g} {:.4g}), first corner ({:.1f} {:.1f} {:.1f}) color {:08X}; stage 0 by NGL {:08X}; color "
                    "mask {:X}{}",
                    g_frames, texture_at, quads, first_index, e.soft ? "soft" : "plain", c14[0], c14[1], first[0],
                    first[1], first[2], color, stage_zero, color_mask,
                    (color_mask & 7u) == 0 && REXCVAR_GET(cod3_debug_ngl_side_effect_mask) ? " (left out)" : "");
      }
    }
  }
  // [rexglue-native] 2026-11-09 rn8: which shaders each flush is drawn with. The native side draws every flush as the
  // pixel shader 8356D96B does (texture x color, the texture's alpha), but rn5's mace2 had a dust cloud drawn as
  // white-edged boxes and its rain missing (owner 14:03: "smoke effects are boxes"). Each pair of shaders with each
  // texture once (the first 64), with the dump frames' flushes; the first 16 pairs' shaders saved as
  // ngl-side-effect<N>-vs/-ps-*.bin (tools/ps_names.py names their constants).
  if (Watching()) {
    static std::mutex pairs_lock;
    static std::unordered_map<uint64_t, int> pairs;  // vertex shader << 32 | pixel shader -> its number
    static std::unordered_set<uint64_t> seen;        // pair number << 32 | texture
    const uint32_t vs = g::ReadU32(base, device + kDeviceVertexShader);
    const uint32_t ps = g::ReadU32(base, device + kDevicePixelShader);
    const uint32_t texture_at = e.texture[1] & 0x1FFFF000;
    std::lock_guard<std::mutex> hold(pairs_lock);
    const auto pair = pairs.emplace(uint64_t{vs} << 32 | ps, static_cast<int>(pairs.size()));
    if (pair.second && pair.first->second < 16) {
      const std::string prefix = "ngl-side-effect" + std::to_string(pair.first->second);
      SaveShader(base, vs, 0x368, 0x1C, true, prefix);
      SaveShader(base, ps, 0x28, 0x18, false, prefix);
    }
    if (seen.size() < 64 && seen.insert(uint64_t(pair.first->second) << 32 | texture_at).second) {
      uint32_t color0 = 0;
      std::memcpy(&color0, &e.corners[7], 4);
      REXLOG_INFO("cod3: ngl side: effect shaders {} (vertex {:08X}, pixel {:08X}): texture {:08X} {}x{} format {} "
                  "swizzle {:03X}, blend {:08X}, {} quads; c14 ({:.4g} {:.4g}) c16 ({:.4g} {:.4g} {:.4g} {:.4g}) c17 "
                  "({:.4g} {:.4g}){}; soft c12.x {:.4g}; pixel c0 ({:.4g} {:.4g} {:.4g} {:.4g}); first corner ({:.1f} "
                  "{:.1f} {:.1f}) uv ({:.3f} {:.3f}) color {:08X}; corners {}",
                  pair.first->second, vs, ps, texture_at, (e.texture[2] & 0x1FFF) + 1,
                  ((e.texture[2] >> 13) & 0x1FFF) + 1, e.texture[1] & 0x3F, (e.texture[3] >> 1) & 0xFFF,
                  e.blend_control, quads, c14[0], c14[1], c16[0], c16[1], c16[2], c16[3], c17[0], c17[1],
                  flipbook ? " (a flipbook)" : "", soft[6][0], e.pixel_c0[0], e.pixel_c0[1], e.pixel_c0[2],
                  e.pixel_c0[3], e.corners[0], e.corners[1], e.corners[2], e.corners[3], e.corners[4], color0,
                  Words(base, vertices + 4 * kEffectCornerWords * first_corner, kEffectCornerWords));
      // [rexglue-native] 2026-11-09 rn11: rn10b still had white-edged dark squares natively (The Mace, frame 3000).
      // The first quad's four corners (place, texture coordinates after c17, alpha), the texture fetch constant's six
      // words (its address modes and filters), and the texture's top level saved as ngl-side-fxtex-<address>.ppm with
      // its alpha as ngl-side-fxtex-<address>-alpha.ppm.
      std::string corners_text;
      for (uint32_t c = 0; c < 4; ++c) {
        const float* k = &e.corners[static_cast<size_t>(c) * NGL_VK_CORNER_FLOATS];
        uint32_t color = 0;
        std::memcpy(&color, &k[7], 4);
        char item[96];
        std::snprintf(item, sizeof item, "%s(%.1f %.1f %.1f uv %.3f %.3f a %u)", c ? " " : "", k[0], k[1], k[2], k[3],
                      k[4], color >> 24);
        corners_text += item;
      }
      REXLOG_INFO("cod3: ngl side: effect texture {:08X}: fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}; first quad {}",
                  texture_at, e.texture[0], e.texture[1], e.texture[2], e.texture[3], e.texture[4], e.texture[5],
                  corners_text);
      const auto save = [texture_at](const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height,
                                     const char* suffix) {
        std::vector<uint8_t> rgba(bgra.size()), alpha(bgra.size());
        for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
          rgba[i] = bgra[i + 2];
          rgba[i + 1] = bgra[i + 1];
          rgba[i + 2] = bgra[i];
          alpha[i] = alpha[i + 1] = alpha[i + 2] = bgra[i + 3];
        }
        char name[64];
        std::snprintf(name, sizeof name, "ngl-side-fxtex-%08X%s.ppm", texture_at, suffix);
        SavePpm(ScreenshotFolder() / name, rgba.data(), width, height);
        std::snprintf(name, sizeof name, "ngl-side-fxtex-%08X%s-alpha.ppm", texture_at, suffix);
        SavePpm(ScreenshotFolder() / name, alpha.data(), width, height);
      };
      std::vector<uint8_t> bgra;
      uint32_t width = 0, height = 0;
      std::string why;
      if (DecodeFetch(base, e.texture, bgra, width, height, why)) save(bgra, width, height, "");
      // rn12: a small packed texture's whole packed tile (the fetch constant's pitch, square), to see where its top
      // level really is
      const bool packed = ((e.texture[5] >> 11) & 1) != 0;
      const uint32_t tile = std::min<uint32_t>(256, std::max<uint32_t>(32, ((e.texture[0] >> 22) & 0x1FF) * 32));
      if (packed && width <= 16 && height <= 16 && DecodeFetchAt(base, e.texture, 0, 0, tile, tile, bgra)) {
        save(bgra, tile, tile, "-tile");
      }
    }
  }
  if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
    REXLOG_INFO("cod3: ngl side: effect flush: {} quads from index {} (corners at {:08X}, byte order {}), texture {}x{} "
                "format {}, blend {:08X}; first corner ({:.1f} {:.1f} {:.1f}) uv ({:.3f} {:.3f}); c14 ({:.4g} {:.4g} "
                "{:.4g} {:.4g}) c16 ({:.4g} {:.4g} {:.4g} {:.4g}) c17 ({:.4g} {:.4g} {:.4g} {:.4g}){}; "
                "soft distance c12.x {:.4g}, the first corner's scale {:.4g}; pixel c0 ({:.4g} {:.4g} {:.4g} {:.4g}){}",
                quads, first_index, vertices, endian, (e.texture[2] & 0x1FFF) + 1, ((e.texture[2] >> 13) & 0x1FFF) + 1,
                e.texture[1] & 0x3F, e.blend_control, e.corners[0], e.corners[1], e.corners[2], e.corners[3],
                e.corners[4], c14[0], c14[1], c14[2], c14[3], c16[0], c16[1], c16[2], c16[3], c17[0], c17[1], c17[2],
                c17[3], flipbook ? ", a flipbook" : "", soft[6][0], e.corners[5], e.pixel_c0[0], e.pixel_c0[1],
                e.pixel_c0[2], e.pixel_c0[3], e.reversed ? "; depth backward" : "");
  }
  // rn42g: a flush that writes none of the frame's colors (RB_COLOR_MASK without its red, green and blue bits; the
  // tank smoke's two effects write the frame's alpha alone) shows nothing in the game's colors: not kept.
  if (REXCVAR_GET(cod3_debug_ngl_side_effect_mask) && (color_mask & 7u) == 0) return;
  std::lock_guard<std::mutex> lock(g_effect_mutex);
  const uint64_t list = e.list;
  g_effects.erase(std::remove_if(g_effects.begin(), g_effects.end(),
                                 [list](const EffectDraw& kept) { return kept.list + 1 < list; }),
                  g_effects.end());
  if (g_effects.size() < kEffectMaxDraws) {
    g_effects.push_back(std::move(e));
  } else {  // rn42n: said once, so a level that still fills it shows in the log
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
      REXLOG_WARN("cod3: ngl side: more than {} effect flushes in two lists; the rest of list {} isn't drawn",
                  kEffectMaxDraws, list);
  }
}
#endif

#if defined(COD3_NGL_VK)
// [rexglue-native] 2026-11-09 rn16: the rain (The Mace's storm; rn14 found its draw, rn15 read it). sub_820D0428 draws
// it as one strip (66 corners in The Mace) of a cylinder around the camera: each corner (x, y) on the unit circle, z 0
// at the bottom and 1 at the top, w the way around (0 to 1). Its vertex shader A5F3656820FFD606 (constants
// WorldToScreen c0-c3, Stretch c4, Thickness c5, CrossHatch c6, Color c7; its literals 0.5, 100, 4000, 1) puts a
// corner at (100 x, 100 y, 4000 (z - 0.5)) through c0-c3 and hands the pixel shader two sets of texture coordinates,
// (w Thickness + z CrossHatch, z Stretch) and (w Thickness - z CrossHatch, z Stretch), and 2 Color x (1 - z) (its
// "adds" doubles c7.x; rn16b drew it with Color alone, half as bright as the game's rain). Its pixel
// shader 14997E3ECCF3DB8F (Offset c0, Density c1, DepthRange c2, InvScreenSize c3, RainTexture s0, ZBuffer s1; its
// literals 9, 3, 8) looks at the rain texture four times: the second set x 9 and x 3, the first x 8 and x 2, each
// with Offset added to v (the fall). It sums color x alpha and alpha, times that color and saturate(the scene's depth
// x DepthRange.x + DepthRange.y) (no rain over what's near), plus Density. Blended one / one minus source alpha,
// without the depth test. Natively (RainDraws): the cylinder drawn four times, one look each, blended by alpha (four
// over each other come to the game's sum where the streaks don't overlap), depth-tested, and faded by ngl_vk's soft
// depth with -DepthRange.x as the scale (rn16d): saturate((the scene's depth - the rain's) x 27.1), where the game has
// saturate(1.14 - 27.1 x the scene's depth) (its backward depth): the same where the rain's own depth is 1.14 / 27.1,
// 0.042 backward. rn16d left the rain's depth where its cylinder stands, which isn't that depth (the cylinder runs
// 4000 up and down), and drew rain over a near soldier the game fades (rn16c and rn16d). rn17 sets every rain pixel's
// own depth to that one depth (1 + DepthRange.y / DepthRange.x turned over: 0.958), so the fade is the game's
// everywhere; the depth test then hides the rain only where that fade is 0 anyway.
constexpr uint32_t kRainMaxCorners = 4096;
constexpr float kRainRadius = 100.0f;   // the vertex shader's literals
constexpr float kRainHeight = 4000.0f;
struct RainDraw {
  float columns[16] = {};  // the matrix c0..c3 makes, as its rows (as EffectDraw's columns: clip x = row 0 . corner)
  float stretch = 0.0f, thickness = 0.0f, cross = 0.0f, color = 0.0f;  // c4.x .. c7.x
  float offset = 0.0f, density = 0.0f;                                 // the pixel shader's c0.x and c1.x
  float depth_range = 0.0f;                                            // its c2.x (DepthRange's scale)
  float depth_offset = 0.0f;                                           // rn17: its c2.y (DepthRange's offset)
  uint32_t texture[6] = {};                                            // fetch constant 0
  bool reversed = false;  // c0..c3's depth runs backward (as KeepEffectDraw's reversed_screen)
  uint64_t list = 0;
  std::vector<float> corners;  // x, y, z, w a corner
};
std::mutex g_rain_mutex;
std::vector<RainDraw> g_rain;
#endif

#if defined(COD3_NGL_VK) && !defined(COD3_MP)
// rn16: the rain's draw, kept as KeepEffectDraw keeps the effects (on the frames a picture uses), for RainDraws. It is
// known by its shaders' constant tables (the vertex shader's names CrossHatch, the pixel shader's RainTexture).
void KeepRainDraw(uint8_t* base) {
  if (!Live() && g_frames % DumpEvery() != DumpEvery() - 1) return;
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  if (!ShaderNames(base, g::ReadU32(base, device + kDeviceVertexShader), 0x368, "CrossHatch") ||
      !ShaderNames(base, g::ReadU32(base, device + kDevicePixelShader), 0x28, "RainTexture")) {
    return;
  }
  const uint32_t vertex_at = device + kDeviceFetchConstants + 8 * kSkyVertexFetch;
  const uint32_t word0 = g::ReadU32(base, vertex_at), word1 = g::ReadU32(base, vertex_at + 4);
  const uint32_t vertices = GuestAddress(word0 & 0x1FFFFFFC);
  const uint32_t count = ((word1 >> 2) & 0xFFFFFF) / 4, endian = word1 & 3;
  static std::atomic<int> refused{0}, shown{0};
  if ((word0 & 3) != 3 || !Physical(vertices) || count < 4 || count > kRainMaxCorners ||
      (g::ReadU32(base, device + kDeviceFetchConstants) & 3) != 2) {
    if (refused.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_INFO("cod3: ngl side: rain left out: vertex fetch 95 {:08X} {:08X}, {} corners", word0, word1, count);
    }
    return;
  }
  RainDraw r;
  // rn16b: the rain's vertex shader takes c0..c3 as the matrix's columns (x c0 + y c1 + z c2 + c3, by mul and mad),
  // the effects' as its rows (dp4 each); kept here as rows, as the effects' are, for the code that follows
  // (rn16's first build used them as rows: its rain landed nowhere on the screen)
  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      r.columns[4 * j + i] = g::ReadF32(base, device + kDeviceVertexConstants + 16 * i + 4 * j);
    }
  }
  r.stretch = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 4);
  r.thickness = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 5);
  r.cross = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 6);
  r.color = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 7);
  r.offset = g::ReadF32(base, device + kDevicePixelConstants);
  r.density = g::ReadF32(base, device + kDevicePixelConstants + 16);
  r.depth_range = g::ReadF32(base, device + kDevicePixelConstants + 32);
  r.depth_offset = g::ReadF32(base, device + kDevicePixelConstants + 36);  // rn17
  for (uint32_t i = 0; i < 6; ++i) r.texture[i] = g::ReadU32(base, device + kDeviceFetchConstants + 4 * i);
  {  // as KeepEffectDraw's reversed_screen: two points along the view (w 10 and 1000) and their depths z / w
    const float* z_row = &r.columns[8];
    const float* w_row = &r.columns[12];
    const float along = w_row[0] * w_row[0] + w_row[1] * w_row[1] + w_row[2] * w_row[2];
    const float z_along = z_row[0] * w_row[0] + z_row[1] * w_row[1] + z_row[2] * w_row[2];
    const auto screen_depth = [&](float w) { return ((w - w_row[3]) / along * z_along + z_row[3]) / w; };
    r.reversed = std::isfinite(along) && along > 0.0f && screen_depth(1000.0f) < screen_depth(10.0f);
  }
  r.corners.resize(static_cast<size_t>(4) * count);
  bool numbers = std::isfinite(r.stretch) && std::isfinite(r.thickness) && std::isfinite(r.cross) &&
                 std::isfinite(r.color) && std::isfinite(r.offset);
  for (uint32_t i = 0; i < 4 * count && numbers; ++i) {
    r.corners[i] = VertexFloat(base, vertices + 4 * i, endian);
    numbers = std::isfinite(r.corners[i]);
  }
  if (!numbers) {
    if (refused.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_INFO("cod3: ngl side: rain left out: a corner or constant isn't a number");
    }
    return;
  }
  r.list = g_list_sends.load(std::memory_order_relaxed);
  if (shown.fetch_add(1, std::memory_order_relaxed) < 4) {
    REXLOG_INFO("cod3: ngl side: rain: {} corners, stretch {:.4g}, thickness {:.4g}, cross-hatch {:.4g}, color "
                "{:.4g}, offset {:.4g}, density {:.4g}, depth range {:.4g} {:.4g}, texture {:08X}{}; corner 1 ({:.3f} "
                "{:.3f} {:.3f} {:.3f})",
                count, r.stretch, r.thickness, r.cross, r.color, r.offset, r.density, r.depth_range, r.depth_offset,
                r.texture[1] & 0x1FFFF000, r.reversed ? ", depth backward" : "", r.corners[4], r.corners[5],
                r.corners[6], r.corners[7]);
  }
  std::lock_guard<std::mutex> lock(g_rain_mutex);
  const uint64_t list = r.list;
  g_rain.erase(std::remove_if(g_rain.begin(), g_rain.end(),
                              [list](const RainDraw& kept) { return kept.list + 1 < list; }),
               g_rain.end());
  if (g_rain.size() < 8) g_rain.push_back(std::move(r));
}

// [rexglue-native] 2026-11-09 rn15: The Mace's rain. rn14 left the game's draws from one call site at a time out of its
// own picture: only the draws returning at 0x820C7940 (the draw in sub_820C78D0, the plain draw helper that SpeedTree's
// leaves use too) took the rain away, and the trees kept their leaves. One caller is sub_820D0428: it sets two textures
// (its settings +72 and +76), their sampler states and its constants, then draws one strip (type 6) of [0x82909D40]
// from the vertex buffer at [0x82909D30] in the format at 0x82909D34; its settings start at 0x82909D70 (+0: on). The
// native picture has nothing for these draws yet. ProbePlainDraw logs each pair of shaders drawn from there once (the
// first 16): whether vertex fetch 95 holds that vertex buffer, the count, the format and its declaration, the first
// vertices, the constants, the depth and blend state and the textures (saved as ngl-side-plain<N>-tex<slot>.ppm, with
// -alpha.ppm), and saves the shaders as ngl-side-plain<N>-vs/-ps (tools/ps_names.py matches them to a shader dump).
constexpr uint32_t kPlainDrawReturn = 0x820C7940;
constexpr uint32_t kRainBuffer = 0x82909D30;
constexpr uint32_t kRainFormat = 0x82909D34;
constexpr uint32_t kRainCount = 0x82909D40;
constexpr uint32_t kRainSettings = 0x82909D70;
constexpr uint32_t kDeviceDepthControl = 0x28B4;  // register 0x2200; kDeviceBlendControl0 follows it

void ProbePlainDraw(uint8_t* base) {
  static std::mutex pairs_lock;
  static std::unordered_map<uint64_t, int> pairs;  // vertex shader << 32 | pixel shader -> its number
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  const uint32_t vs = g::ReadU32(base, device + kDeviceVertexShader);
  const uint32_t ps = g::ReadU32(base, device + kDevicePixelShader);
  int number = 0;
  {
    std::lock_guard<std::mutex> hold(pairs_lock);
    const uint64_t key = uint64_t{vs} << 32 | ps;
    if (pairs.size() >= 16 || pairs.count(key)) return;
    number = static_cast<int>(pairs.size());
    pairs.emplace(key, number);
  }
  const std::string prefix = "ngl-side-plain" + std::to_string(number);
  const auto heap = [](uint32_t a) { return !(a & 3) && a >= 0x40000000u && a < 0x80000000u; };
  const uint32_t vertex_at = device + kDeviceFetchConstants + 8 * kSkyVertexFetch;
  const uint32_t word0 = g::ReadU32(base, vertex_at), word1 = g::ReadU32(base, vertex_at + 4);
  const uint32_t vertices = (word0 & 3) == 3 ? GuestAddress(word0 & 0x1FFFFFFC) : 0;
  const uint32_t buffer = g::ReadU32(base, kRainBuffer);
  const uint32_t rain_vertices =
      Readable(buffer) || heap(buffer) ? GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & 0x1FFFFFFC) : 0;
  const uint32_t declaration = g::ReadU32(base, kRainFormat + 8);
  REXLOG_INFO("cod3: ngl side: {}: plain draw (vertex shader {:08X}, pixel {:08X}); vertex fetch 95 {:08X} {:08X} "
              "({:08X}, {} words, byte order {}); {} the rain's buffer {:08X} ({:08X}); the rain's count {}, format {}, "
              "declaration {:08X}: {}; settings: {} / {} / {}",
              prefix, vs, ps, word0, word1, vertices, (word1 >> 2) & 0xFFFFFF, word1 & 3,
              vertices && vertices == rain_vertices ? "it is" : "not", buffer, rain_vertices,
              g::ReadU32(base, kRainCount), Words(base, kRainFormat, 3), declaration,
              heap(declaration) || Readable(declaration) ? Words(base, declaration, 16) : std::string("-"),
              Words(base, kRainSettings, 8), Words(base, kRainSettings + 0x20, 8), Words(base, kRainSettings + 0x40, 8));
  if (Physical(vertices)) {
    REXLOG_INFO("cod3: ngl side: {}: first vertex words: {} / {} / {} / {}", prefix, Words(base, vertices, 8),
                Words(base, vertices + 0x20, 8), Words(base, vertices + 0x40, 8), Words(base, vertices + 0x60, 8));
  }
  REXLOG_INFO("cod3: ngl side: {}: depth and blend {}; vertex constants:{}", prefix,
              Words(base, device + kDeviceDepthControl, 12),
              ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  REXLOG_INFO("cod3: ngl side: {}: pixel constants:{}", prefix,
              ConstantsText(base, device + kDevicePixelConstants, nullptr));
  for (uint32_t slot = 0; slot < 4; ++slot) {
    uint32_t fetch[6];
    for (uint32_t i = 0; i < 6; ++i) {
      fetch[i] = g::ReadU32(base, device + kDeviceFetchConstants + kFetchConstantBytes * slot + 4 * i);
    }
    if ((fetch[0] & 3) != 2) continue;
    std::vector<uint8_t> bgra;
    uint32_t width = 0, height = 0;
    std::string why;
    const bool decoded = DecodeFetch(base, fetch, bgra, width, height, why);
    REXLOG_INFO("cod3: ngl side: {}: texture {}: fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}: {}x{} format {}{}{}",
                prefix, slot, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5], (fetch[2] & 0x1FFF) + 1,
                ((fetch[2] >> 13) & 0x1FFF) + 1, fetch[1] & 0x3F, decoded ? "" : ", not decoded: ", why);
    if (!decoded) continue;
    std::vector<uint8_t> rgba(bgra.size()), alpha(bgra.size());
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
      rgba[i] = bgra[i + 2];
      rgba[i + 1] = bgra[i + 1];
      rgba[i + 2] = bgra[i];
      alpha[i] = alpha[i + 1] = alpha[i + 2] = bgra[i + 3];
    }
    const std::string stem = prefix + "-tex" + std::to_string(slot);
    SavePpm(ScreenshotFolder() / (stem + ".ppm"), rgba.data(), width, height);
    SavePpm(ScreenshotFolder() / (stem + "-alpha.ppm"), alpha.data(), width, height);
  }
  SaveShader(base, vs, 0x368, 0x1C, true, prefix);
  SaveShader(base, ps, 0x28, 0x18, false, prefix);
}
#endif

#if defined(COD3_NGL_VK)
std::unordered_map<uint64_t, uint32_t> g_sky_meshes;    // vertices << 32 | indices -> mesh (0: not drawable)
std::unordered_map<uint64_t, std::vector<float>> g_sky_corners;  // the same key -> the mesh's corners (side102)
std::unordered_map<uint64_t, std::vector<uint16_t>> g_sky_indices;  // the same key -> the mesh's indices (side175)
std::unordered_map<uint64_t, uint32_t> g_sky_textures;  // texture address << 32 | size word -> texture (0: none)

uint32_t SkyMesh(uint8_t* base, const SkyDraw& s, std::string& why) {
  const uint64_t key = uint64_t{s.vertices} << 32 | s.indices;
  const auto found = g_sky_meshes.find(key);
  if (found != g_sky_meshes.end()) {
    if (!found->second) why = "left out before";
    return found->second;
  }
  uint32_t& mesh = g_sky_meshes[key];
  const uint32_t count = s.vertex_words / kSkyVertexWords;
  std::vector<float> corners(static_cast<size_t>(count) * NGL_VK_CORNER_FLOATS, 0.0f);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = s.vertices + 4 * kSkyVertexWords * i;
    float* c = &corners[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    for (uint32_t k = 0; k < 5; ++k) c[k] = VertexFloat(base, at + 4 * k, s.vertex_endian);
    c[5] = c[3];
    c[6] = c[4];
    uint32_t color = GpuU32(base, at + 20);  // a D3DCOLOR, 0xAARRGGBB
    if (s.vertex_endian == 0) color = __builtin_bswap32(color);
    std::memcpy(&c[7], &color, 4);
    for (uint32_t k = 0; k < 5; ++k) {
      if (!std::isfinite(c[k])) {
        why = "a corner isn't a number";
        // side140: which, the first times (side144: and its words as they are; side146: once a layer, the depth
        // texture's draws, which aren't sky, not at all)
        static std::unordered_set<uint32_t> shown;
        if ((s.texture[1] & 0x3F) != 23 && shown.size() < 24 && shown.insert(s.vertices).second) {
          REXLOG_INFO("cod3: ngl side: sky layer at {:08X} left out: corner {} of {} isn't a number ({:08X} {:08X} "
                      "{:08X} {:08X} {:08X} {:08X}, byte order {}; {} words, texture format {})", s.vertices, i, count,
                      GpuU32(base, at), GpuU32(base, at + 4), GpuU32(base, at + 8), GpuU32(base, at + 12),
                      GpuU32(base, at + 16), GpuU32(base, at + 20), s.vertex_endian, s.vertex_words,
                      s.texture[1] & 0x3F);
        }
        return 0;
      }
    }
  }
  // the indices, in the byte order that keeps them all inside the corners
  const uint32_t index_count = s.index_bytes / 2 / 3 * 3;
  std::vector<uint16_t> indices(index_count);
  uint32_t lowest[2] = {~0u, ~0u}, highest[2] = {0, 0};
  for (int order = 0; order < 2; ++order) {
    // side140: in the byte order that keeps them inside the corners, as they are or counted from the lowest one
    // (mayenne's sky layers were all left out: their indices may count from where the draw's corners start in a
    // shared buffer)
    for (uint32_t i = 0; i < index_count; ++i) {
      const uint8_t* p = GpuPointer(base, s.indices + 2 * i);
      const uint32_t value = order == 0 ? (uint32_t{p[0]} << 8 | p[1]) : (uint32_t{p[1]} << 8 | p[0]);
      lowest[order] = std::min(lowest[order], value);
      highest[order] = std::max(highest[order], value);
    }
    for (const uint32_t from : {0u, lowest[order]}) {
      if (index_count == 0 || from > lowest[order] || highest[order] - from >= count) continue;
      for (uint32_t i = 0; i < index_count; ++i) {
        const uint8_t* p = GpuPointer(base, s.indices + 2 * i);
        const uint32_t value = order == 0 ? (uint32_t{p[0]} << 8 | p[1]) : (uint32_t{p[1]} << 8 | p[0]);
        indices[i] = static_cast<uint16_t>(value - from);
      }
      mesh = ngl_vk_mesh(corners.data(), count, indices.data(), index_count);
      if (!mesh) {
        why = "ngl_vk has no room";
        static int shown = 0;  // side144
        if (shown < 8) {
          ++shown;
          REXLOG_INFO("cod3: ngl side: sky layer at {:08X} left out: ngl_vk made no mesh of {} corners and {} indices",
                      s.vertices, count, index_count);
        }
      }
      if (mesh) {
        g_sky_corners[key] = std::move(corners);
        g_sky_indices[key] = indices;
      }
      return mesh;
    }
  }
  why = "indices past the corners";
  static std::unordered_set<uint32_t> shown;  // side146: once a layer
  if (shown.size() < 24 && shown.insert(s.vertices).second) {
    REXLOG_INFO("cod3: ngl side: sky layer at {:08X} left out: {} corners, {} indices from {} to {} (or {} to {} the "
                "other way round)", s.vertices, count, index_count, lowest[0], highest[0], lowest[1], highest[1]);
  }
  return 0;
}

// side175: where a sky layer's corners and triangles land on the screen, on a frame that is saved: its corners ahead of
// the eye and on the screen, how far they reach (x and y from -1 to 1 across the screen), and its triangles ahead of
// the eye, flat (no area on the screen) and turned away (the other winding)
void SkyLayerPlace(uint64_t frame, uint32_t layer, const SkyDraw& s, const float* view_projection) {
  const uint64_t key = uint64_t{s.vertices} << 32 | s.indices;
  const auto corners = g_sky_corners.find(key);
  const auto indices = g_sky_indices.find(key);
  if (corners == g_sky_corners.end() || indices == g_sky_indices.end()) return;
  const std::vector<float>& c = corners->second;
  if (c.size() < NGL_VK_CORNER_FLOATS) return;
  const uint32_t count = static_cast<uint32_t>(c.size() / NGL_VK_CORNER_FLOATS);
  std::vector<float> screen(static_cast<size_t>(count) * 3);  // x / w, y / w, w
  uint32_t ahead = 0, shown = 0;
  float low[2] = {1e30f, 1e30f}, high[2] = {-1e30f, -1e30f};
  for (uint32_t i = 0; i < count; ++i) {
    const float* p = &c[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    float clip[4];
    for (int j = 0; j < 4; ++j)
      clip[j] = p[0] * view_projection[j] + p[1] * view_projection[4 + j] + p[2] * view_projection[8 + j] +
                view_projection[12 + j];
    screen[3 * i + 2] = clip[3];
    if (!(clip[3] > 0.0f)) continue;
    ++ahead;
    screen[3 * i] = clip[0] / clip[3];
    screen[3 * i + 1] = clip[1] / clip[3];
    for (int j = 0; j < 2; ++j) {
      low[j] = std::min(low[j], screen[3 * i + j]);
      high[j] = std::max(high[j], screen[3 * i + j]);
    }
    if (std::fabs(screen[3 * i]) <= 1.0f && std::fabs(screen[3 * i + 1]) <= 1.0f) ++shown;
  }
  uint32_t color_low[4] = {255, 255, 255, 255}, color_high[4] = {0, 0, 0, 0};  // side177: bytes as stored
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t color = 0;
    std::memcpy(&color, &c[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS + 7], 4);
    for (int k = 0; k < 4; ++k) {
      const uint32_t byte = (color >> (8 * k)) & 0xFF;
      color_low[k] = std::min(color_low[k], byte);
      color_high[k] = std::max(color_high[k], byte);
    }
  }
  const std::vector<uint16_t>& t = indices->second;
  uint32_t triangles_ahead = 0, flat = 0, away = 0;
  for (size_t i = 0; i + 2 < t.size(); i += 3) {
    if (t[i] >= count || t[i + 1] >= count || t[i + 2] >= count) continue;
    const float* a = &screen[3 * t[i]];
    const float* b = &screen[3 * t[i + 1]];
    const float* e = &screen[3 * t[i + 2]];
    if (!(a[2] > 0.0f && b[2] > 0.0f && e[2] > 0.0f)) continue;
    ++triangles_ahead;
    const float area = (b[0] - a[0]) * (e[1] - a[1]) - (b[1] - a[1]) * (e[0] - a[0]);
    if (std::fabs(area) < 1e-6f) ++flat;
    else if (area < 0.0f) ++away;
  }
  REXLOG_INFO("cod3: ngl side: frame {}: sky layer {}: {} corners, {} ahead of the eye, {} on the screen, x {:.2f} to "
              "{:.2f}, y {:.2f} to {:.2f}; {} triangles, {} ahead ({} flat, {} the other way round); first corner "
              "({:.1f} {:.1f} {:.1f}) uv ({:.3f} {:.3f}); corner color bytes from {} {} {} {} to {} {} {} {}",
              frame, layer, count, ahead, shown, low[0], high[0], low[1], high[1], t.size() / 3, triangles_ahead, flat,
              away, c[0], c[1], c[2], c[3], c[4], color_low[0], color_low[1], color_low[2], color_low[3],
              color_high[0], color_high[1], color_high[2], color_high[3]);
}

std::vector<ngl_vk_mesh_draw> SkyDraws(uint8_t* base, uint64_t frame) {
  std::vector<SkyDraw> sky;
  const uint64_t now = g_list_sends.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_sky_mutex);
    for (const SkyDraw& s : g_sky) {
      if (s.list + kSkyRecent >= now) sky.push_back(s);
    }
  }
  std::vector<ngl_vk_mesh_draw> draws;
  std::string text;
  const uint32_t shown_layers = static_cast<uint32_t>(REXCVAR_GET(cod3_debug_ngl_side_sky_layers));
  bool hidden = false;  // side171: a layer left out by cod3_debug_ngl_side_sky_layers
  uint32_t layer = 0;
  for (const SkyDraw& s : sky) {
    const bool shown = layer >= 32 || ((shown_layers >> layer) & 1u) != 0;
    ++layer;
    if (!shown) {
      text += " (left out by cod3_debug_ngl_side_sky_layers)";
      hidden = true;
      continue;
    }
    std::string why;
    const uint32_t mesh = SkyMesh(base, s, why);
    const uint64_t texture_key = uint64_t{s.texture[1] & 0x1FFFF000} << 32 | s.texture[2];
    auto texture = g_sky_textures.find(texture_key);
    if (texture == g_sky_textures.end()) {
      std::string texture_why;
      t_sky_texture = true;  // side192
      // rn10: the layers are drawn opaque or blended, never alpha-tested: the game's own levels
      const uint32_t id = MakeTexture(base, s.texture, s.texture[1] & 0x1FFFF000, texture_why, true);
      t_sky_texture = false;
      if (!id) REXLOG_INFO("cod3: ngl side: sky texture {:08X} not drawn: {}", s.texture[1] & 0x1FFFF000, texture_why);
      texture = g_sky_textures.emplace(texture_key, id).first;
    }
    if (!mesh || !texture->second) {
      text += " (left out: " + (mesh ? std::string("no texture") : why) + ")";
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = mesh;
    d.texture = texture->second;
    const bool flat = layer <= 32 && ((static_cast<uint32_t>(REXCVAR_GET(cod3_debug_ngl_side_sky_flat)) >>
                                       (layer - 1)) & 1u) != 0;
    if (flat) {
      d.texture = 0;
      d.tint = 0xFF00FFu;
    }
    if ((s.drift[0] != 0.0f || s.drift[1] != 0.0f) && REXCVAR_GET(cod3_debug_ngl_side_sky_drift)) {
      // side102: this frame's corners, moved by the layer's drift
      const auto kept = g_sky_corners.find(uint64_t{s.vertices} << 32 | s.indices);
      if (kept != g_sky_corners.end()) {
        std::vector<float> moved = kept->second;
        for (size_t at = 0; at + NGL_VK_CORNER_FLOATS <= moved.size(); at += NGL_VK_CORNER_FLOATS) {
          moved[at + 3] += s.drift[0];
          moved[at + 4] += s.drift[1];
        }
        d.corners = ngl_vk_frame_corners(moved.data(), static_cast<uint32_t>(moved.size() / NGL_VK_CORNER_FLOATS));
      }
    }
    d.shading = NGL_VK_SHADE_UNLIT;
    const bool base_blended = REXCVAR_GET(cod3_debug_ngl_side_sky_base_blended);
    const bool first = draws.empty() && !hidden && !base_blended;  // a layer after a hidden one keeps its own blend
    d.alpha = first ? NGL_VK_ALPHA_OPAQUE : NGL_VK_ALPHA_BLEND;
    d.blend = NGL_VK_BLEND_ALPHA;
    const char* how = first ? "opaque" : "blended";
    {  // side98: the layer's own blend state when the draw had one (Xenos factors: 0 zero, 1 one, 6 source alpha,
       // 7 one minus source alpha; operation 0 add)
      const uint32_t source = s.blend_control & 0x1F, operation = (s.blend_control >> 5) & 7;
      const uint32_t destination = (s.blend_control >> 8) & 0x1F;
      if (s.blend_control && source == 1 && destination == 0) {
        d.alpha = base_blended ? NGL_VK_ALPHA_BLEND : NGL_VK_ALPHA_OPAQUE;
        how = base_blended ? "opaque, through the blended pipeline" : "opaque";
      } else if (s.blend_control && operation == 0 && destination == 1) {
        d.alpha = NGL_VK_ALPHA_BLEND;
        d.blend = NGL_VK_BLEND_ADD;
        how = "added";
      } else if (s.blend_control && operation == 0 && source == 6 && destination == 7) {
        d.alpha = NGL_VK_ALPHA_BLEND;
        how = "blended";
      }
    }
    d.world[0] = d.world[5] = d.world[10] = d.world[15] = 1.0f;
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) d.view_projection[4 * row + column] = s.columns[4 * column + row];
      d.view_projection[4 * row + 2] = d.view_projection[4 * row + 3];  // z = w: at the far plane
    }
    if (!g_quiet) SkyLayerPlace(frame, layer, s, d.view_projection);
    char item[112];
    std::snprintf(item, sizeof item, " %s%s %ux%u (%08X, drift %.3f %.3f)", how, flat ? " flat" : "",
                  (s.texture[2] & 0x1FFF) + 1, ((s.texture[2] >> 13) & 0x1FFF) + 1, s.blend_control, s.drift[0],
                  s.drift[1]);
    text += item;
    draws.push_back(d);
  }
  if (!g_quiet)
    REXLOG_INFO("cod3: ngl side: frame {}: sky: {} of {} layers drawn:{}", frame, draws.size(), sky.size(),
                text.empty() ? std::string(" none") : text);
  return draws;
}
// side104: the last frame's effect flushes as blended draws of one shared quad mesh (kEffectMaxQuads quads of the
// game's corner order), each with its own corners for this frame; fogged as the world (fog_from: a world draw).
uint32_t g_effect_mesh = 0;
std::unordered_map<uint64_t, uint32_t> g_effect_textures;  // texture address << 32 | size word -> texture (0: none)

// rn42c (owner 09:13: "smoke coming from the tank's barrel is square"; rn42a/rn42b: natively white-framed squares where
// the game draws soft puffs): each effect texture as decoded, once, up to 48 (debug only, with cod3_debug_ngl_side):
// two pictures in the screenshot folder, ngl-side-fx-<address>-color.ppm and -alpha.ppm (the alpha as grey), and a
// line with its size, format, fetch words, mean color and alpha, how much of it is see-through, and the flush's blend,
// soft flag, quad count, texture coordinate range and corner alpha range (the first flush that made it)
void SaveEffectTexture(uint8_t* base, const EffectDraw& e, uint32_t id) {
  static int saved = 0;
  if (!REXCVAR_GET(cod3_debug_ngl_side) || saved >= 48) return;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!DecodeFetch(base, e.texture, bgra, width, height, why) || bgra.size() < 4) return;
  ++saved;
  const uint32_t address = e.texture[1] & 0x1FFFF000;
  std::vector<uint8_t> color(bgra.size()), alpha(bgra.size());
  uint64_t sum[4] = {};
  size_t clear = 0, solid = 0;
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
    color[i] = bgra[i + 2];
    color[i + 1] = bgra[i + 1];
    color[i + 2] = bgra[i];
    color[i + 3] = 255;
    alpha[i] = alpha[i + 1] = alpha[i + 2] = bgra[i + 3];
    alpha[i + 3] = 255;
    for (int k = 0; k < 4; ++k) sum[k] += bgra[i + k];
    clear += bgra[i + 3] < 16;
    solid += bgra[i + 3] > 239;
  }
  const uint64_t n = bgra.size() / 4;
  char name[64];
  std::snprintf(name, sizeof name, "ngl-side-fx-%08X-color.ppm", address);
  SavePpm(ScreenshotFolder() / name, color.data(), width, height);
  std::snprintf(name, sizeof name, "ngl-side-fx-%08X-alpha.ppm", address);
  SavePpm(ScreenshotFolder() / name, alpha.data(), width, height);
  float low_u = 1e9f, low_v = 1e9f, high_u = -1e9f, high_v = -1e9f;
  uint32_t low_a = 255, high_a = 0;
  for (size_t c = 0; c + NGL_VK_CORNER_FLOATS <= e.corners.size(); c += NGL_VK_CORNER_FLOATS) {
    low_u = std::min(low_u, e.corners[c + 3]);
    high_u = std::max(high_u, e.corners[c + 3]);
    low_v = std::min(low_v, e.corners[c + 4]);
    high_v = std::max(high_v, e.corners[c + 4]);
    uint32_t packed = 0;
    std::memcpy(&packed, &e.corners[c + 7], 4);
    low_a = std::min(low_a, packed >> 24);
    high_a = std::max(high_a, packed >> 24);
  }
  REXLOG_INFO("cod3: ngl side: rn42c effect texture {:08X} (ngl_vk {}): {}x{} format {}, fetch {:08X} {:08X} {:08X} "
              "{:08X} {:08X} {:08X}; mean red {} green {} blue {} alpha {}, {}% clear, {}% solid; blend {:08X}, {}, {} "
              "quads, u {:.3f}..{:.3f} v {:.3f}..{:.3f}, corner alpha {}..{}",
              address, id, width, height, e.texture[1] & 0x3F, e.texture[0], e.texture[1], e.texture[2], e.texture[3],
              e.texture[4], e.texture[5], sum[2] / n, sum[1] / n, sum[0] / n, sum[3] / n, 100 * clear / n,
              100 * solid / n, e.blend_control, e.soft ? "soft" : "plain", e.quads, low_u, high_u, low_v, high_v,
              low_a, high_a);
}

std::vector<ngl_vk_mesh_draw> EffectDraws(uint8_t* base, uint64_t frame, const ngl_vk_mesh_draw* fog_from) {
  std::vector<EffectDraw> effects;
  const uint64_t now = g_list_sends.load(std::memory_order_relaxed);
  // side223: the world camera the last frame was drawn with, which is the one these flushes were made with
  const WorldCamera before = g_world_camera_before;
  g_world_camera_before = g_world_camera;
  const bool current_camera = REXCVAR_GET(cod3_native_effects_current_camera) && g_world_camera.ok && before.ok;
  uint32_t moved = 0;
  uint32_t newer = 0, older = 0;  // rn42j: the kept flushes not drawn: of the list sent now or later, of earlier ones
  {
    std::lock_guard<std::mutex> lock(g_effect_mutex);
    for (const EffectDraw& e : g_effects) {
      if (e.list + 1 == now)
        effects.push_back(e);
      else if (e.list >= now)
        ++newer;
      else
        ++older;
    }
  }
  std::vector<ngl_vk_mesh_draw> draws;
  if (effects.empty()) {
    if (g_live_shot)  // rn42j: a live frame saved with no effects of the last list
      REXLOG_INFO("cod3: ngl side: frame {}: effects: none of list {} kept ({} of the list sent now or later, {} "
                  "older)", frame, now - 1, newer, older);
    return draws;
  }
  if (!g_effect_mesh) {
    std::vector<float> corners(static_cast<size_t>(4 * kEffectMaxQuads) * NGL_VK_CORNER_FLOATS, 0.0f);
    std::vector<uint16_t> indices(6 * kEffectMaxQuads);
    for (uint32_t q = 0; q < kEffectMaxQuads; ++q) {
      for (uint32_t k = 0; k < 6; ++k) indices[6 * q + k] = static_cast<uint16_t>(4 * q + g_effect_pattern[k]);
    }
    g_effect_mesh = ngl_vk_mesh(corners.data(), 4 * kEffectMaxQuads, indices.data(), 6 * kEffectMaxQuads);
    if (!g_effect_mesh) {
      REXLOG_INFO("cod3: ngl side: no room in ngl_vk for the effects' quad mesh");
      return draws;
    }
  }
  uint32_t quads = 0, added = 0, untextured = 0, own_fogs = 0, plain = 0;
  // rn42j: cod3_debug_ngl_side_effect_soft (see there); corner_quads counts the soft quads given their corners' depth,
  // tilted those whose corners' distances along the view differ by more than 5%
  const int32_t soft_mode = REXCVAR_GET(cod3_debug_ngl_side_effect_soft_ab) ? ((frame & 1) ? 1 : 2)  // rn42m
                                                                             : REXCVAR_GET(cod3_debug_ngl_side_effect_soft);
  uint32_t corner_quads = 0, tilted = 0;
  std::vector<float> corner_depths;
  std::map<uint32_t, uint32_t> texture_counts;  // rn42m: the drawn flushes by texture, on the live frames saved
  for (const EffectDraw& e : effects) {
    const uint64_t texture_key = uint64_t{e.texture[1] & 0x1FFFF000} << 32 | e.texture[2];
    auto texture = g_effect_textures.find(texture_key);
    if (texture == g_effect_textures.end()) {
      std::string why;
      const uint32_t id = MakeTexture(base, e.texture, e.texture[1] & 0x1FFFF000, why, true);  // rn10: blended
      if (!id) REXLOG_INFO("cod3: ngl side: effect texture {:08X} not drawn: {}", e.texture[1] & 0x1FFFF000, why);
      SaveEffectTexture(base, e, id);  // rn42c (debug only)
      texture = g_effect_textures.emplace(texture_key, id).first;
    }
    if (!texture->second) {
      ++untextured;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = g_effect_mesh;
    d.index_count = 6 * e.quads;
    d.texture = texture->second;
    d.shading = NGL_VK_SHADE_VERTEX_COLOR;
    d.light_once = 1;  // the corners' colors as they are (the pixel shader's texture x color)
    d.alpha = NGL_VK_ALPHA_BLEND;
    // side112: faded near what's behind it, as the game's soft pixel shader does (rn10: only the soft pair's)
    d.soft_depth = e.soft && soft_mode != 0 ? 1 : 0;
    plain += !e.soft;
    // Xenos blend factors: 1 one, 6 source alpha, 7 one minus source alpha; a destination of one adds
    const uint32_t destination = (e.blend_control >> 8) & 0x1F;
    d.blend = destination == 1 ? NGL_VK_BLEND_ADD : NGL_VK_BLEND_ALPHA;
    added += destination == 1;
    d.world[0] = d.world[5] = d.world[10] = d.world[15] = 1.0f;
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) d.view_projection[4 * row + column] = e.columns[4 * column + row];
      // side115: a backward depth turned over (w - z), as SceneCamera does for the world's draws
      if (e.reversed) {
        d.view_projection[4 * row + 2] = d.view_projection[4 * row + 3] - d.view_projection[4 * row + 2];
      }
    }
    // side223: made with last frame's world camera, so drawn with this frame's
    if (current_camera && SameCamera(d.view_projection, before.view_projection)) {
      std::memcpy(d.view_projection, g_world_camera.view_projection, sizeof d.view_projection);
      ++moved;
    }
    // rn42j: the soft pair's own depth blended from the corners (cod3_debug_ngl_side_effect_soft 2): each corner's
    // depth through the camera it is drawn with (ngl_vk's z / w, turned over as above) in the lightmap v, and the
    // fade's scale made negative to say so (mesh.frag's SOFT). A quad with a corner behind the eye or one not faded
    // (1e8) keeps the pixel's depth, as before.
    const float* corners = e.corners.data();
    if (d.soft_depth && soft_mode == 2) {
      corner_depths.assign(e.corners.begin(), e.corners.end());
      const float* m = d.view_projection;
      for (uint32_t q = 0; q < e.quads; ++q) {
        float depth[4], distance[4];
        bool fits = true;
        for (uint32_t k = 0; k < 4 && fits; ++k) {
          const float* out = &corner_depths[static_cast<size_t>(4 * q + k) * NGL_VK_CORNER_FLOATS];
          const float z = m[2] * out[0] + m[6] * out[1] + m[10] * out[2] + m[14];
          const float w = m[3] * out[0] + m[7] * out[1] + m[11] * out[2] + m[15];
          depth[k] = z / w;
          distance[k] = w;
          fits = w > 1e-6f && std::isfinite(depth[k]) && out[5] > 0.0f && out[5] < 1e7f;
        }
        if (!fits) continue;
        for (uint32_t k = 0; k < 4; ++k) {
          float* out = &corner_depths[static_cast<size_t>(4 * q + k) * NGL_VK_CORNER_FLOATS];
          out[5] = -out[5];
          out[6] = depth[k];
        }
        ++corner_quads;
        const float nearest = std::min(std::min(distance[0], distance[1]), std::min(distance[2], distance[3]));
        const float farthest = std::max(std::max(distance[0], distance[1]), std::max(distance[2], distance[3]));
        tilted += farthest - nearest > 0.05f * farthest;
      }
      corners = corner_depths.data();
    }
    d.corners = ngl_vk_frame_corners(corners, 4 * e.quads);
    if (!d.corners) break;  // the frame's room is used up
    // side114: the flush's fog as the game's pixel shader does it, color x c0.w + c0.xyz: ngl_vk's fog
    // mix(color, rgb, amount) with a constant amount (an end before every corner, so the end's amount) of 1 - c0.w and
    // rgb c0.xyz / amount. A c0 that can't be put so (c0.w outside 0..1, or rgb past 1) keeps the world's fog.
    const float* c0 = e.pixel_c0;
    const float amount = 1.0f - c0[3];
    bool own_fog = std::isfinite(c0[0]) && std::isfinite(c0[1]) && std::isfinite(c0[2]) && amount >= 0.0f &&
                   amount <= 1.0f;
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    for (int k = 0; own_fog && k < 3; ++k) {
      rgb[k] = amount > 1e-4f ? c0[k] / amount : 0.0f;
      own_fog = rgb[k] >= -1e-3f && rgb[k] <= 1.001f && (amount > 1e-4f || std::fabs(c0[k]) < 1e-3f);
    }
    if (own_fog) {
      d.fog[0] = -2.0f;  // start and end before the eye: every corner past the end
      d.fog[1] = -1.0f;
      d.fog[2] = amount;
      d.fog_color = UnitByte(rgb[0]) | UnitByte(rgb[1]) << 8 | UnitByte(rgb[2]) << 16 | UnitByte(amount) << 24;
      ++own_fogs;
    } else if (fog_from) {
      std::memcpy(d.fog, fog_from->fog, sizeof d.fog);
      d.fog_color = fog_from->fog_color;
    }
    {  // rn11: cod3_debug_ngl_side_effect_mark, a flat magenta in place of the texture (as the sky's flat layers)
      const int32_t mark = REXCVAR_GET(cod3_debug_ngl_side_effect_mark);
      if (mark != 0 && static_cast<uint32_t>(mark) == (e.texture[1] & 0x1FFFF000)) {
        d.texture = 0;
        d.tint = 0xFF00FFu;
      }
    }
    quads += e.quads;
    draws.push_back(d);
    if (g_live_shot) ++texture_counts[e.texture[1] & 0x1FFFF000u];  // rn42m
  }
  if (g_live_shot) {  // rn42m: beside the GPU's own (CountEffectFrame)
    std::string list;
    for (const auto& [address, count] : texture_counts) {
      char item[24];
      std::snprintf(item, sizeof item, " %08X x%u", address, count);
      list += item;
    }
    REXLOG_INFO("cod3: ngl side: rn42m native effects (frame {}): {} draws, {} quads;{}", frame, draws.size(), quads,
                list);
  }
  if (!g_quiet || g_live_shot)
    REXLOG_INFO("cod3: ngl side: frame {}: effects: {} of {} flushes drawn ({} added, {} without a texture, {} with "
                "their own fog, {} moved to this frame's camera, {} plain), {} quads; rn42j: {} soft quads faded by "
                "their corners' depth ({} tilted; soft mode {}); list {}, {} kept of the list sent now or later, {} "
                "older", frame, draws.size(), effects.size(), added, untextured, own_fogs, moved, plain, quads,
                corner_quads, tilted, soft_mode, now - 1, newer, older);
  return draws;
}

// [rexglue-native] 2026-11-09 rn16: the last list's rain (KeepRainDraw), as the game's shaders draw it (see RainDraw):
// the cylinder once for each of the pixel shader's four looks at the rain texture, each with its own texture
// coordinates (its set of the vertex shader's two, times its scale, plus the fall), white, its alpha the vertex
// shader's 2 Color x (1 - z), blended by alpha after the world and the effects, depth-tested and faded by the scene's
// depth (soft_depth), not fogged (the game's
// rain has no fog). One mesh for each corner count (the strip as a triangle list), its corners given each frame.
std::unordered_map<uint32_t, uint32_t> g_rain_meshes;    // corner count -> mesh (0: none)
std::unordered_map<uint64_t, uint32_t> g_rain_textures;  // texture address << 32 | size word -> texture (0: none)

std::vector<ngl_vk_mesh_draw> RainDraws(uint8_t* base, uint64_t frame) {
  std::vector<ngl_vk_mesh_draw> draws;
  if (!REXCVAR_GET(cod3_native_rain)) return draws;
  std::vector<RainDraw> rain;
  {
    // the last list's, as the effects; when a list's draw was counted with the next list (side224's race), the newest
    // kept from the two lists before, so the rain doesn't blink out for a frame (it falls every frame in the game)
    const uint64_t now = g_list_sends.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_rain_mutex);
    uint64_t pick = 0;
    bool found = false;
    for (const RainDraw& r : g_rain) {
      if (r.list + 1 == now) {
        pick = r.list;
        found = true;
        break;
      }
      if (r.list <= now && r.list + 2 >= now && (!found || r.list > pick)) {
        pick = r.list;
        found = true;
      }
    }
    for (const RainDraw& r : g_rain) {
      if (found && r.list == pick) rain.push_back(r);
    }
  }
  // the pixel shader's four looks: which set of coordinates (0: the first, w Thickness + z CrossHatch; 1: the second,
  // w Thickness - z CrossHatch) and its scale
  struct Look {
    int set;
    float scale;
  };
  static constexpr Look kLooks[4] = {{1, 9.0f}, {0, 8.0f}, {0, 2.0f}, {1, 3.0f}};
  for (const RainDraw& r : rain) {
    const uint32_t count = static_cast<uint32_t>(r.corners.size() / 4);
    uint32_t& mesh = g_rain_meshes[count];
    if (!mesh) {
      std::vector<float> corners(static_cast<size_t>(count) * NGL_VK_CORNER_FLOATS, 0.0f);
      std::vector<uint16_t> indices;
      for (uint32_t i = 0; i + 2 < count; ++i) {  // a strip's triangles (ngl_vk doesn't cull, so their turn is free)
        indices.push_back(static_cast<uint16_t>(i));
        indices.push_back(static_cast<uint16_t>(i + 1));
        indices.push_back(static_cast<uint16_t>(i + 2));
      }
      mesh = ngl_vk_mesh(corners.data(), count, indices.data(), static_cast<uint32_t>(indices.size()));
      static std::atomic<int> no_room{0};
      if (!mesh) {
        if (no_room.fetch_add(1, std::memory_order_relaxed) < 2) {
          REXLOG_INFO("cod3: ngl side: no room in ngl_vk for the rain's mesh ({} corners)", count);
        }
        continue;
      }
    }
    const uint64_t texture_key = uint64_t{r.texture[1] & 0x1FFFF000} << 32 | r.texture[2];
    auto texture = g_rain_textures.find(texture_key);
    if (texture == g_rain_textures.end()) {
      std::string why;
      const uint32_t id = MakeTexture(base, r.texture, r.texture[1] & 0x1FFFF000, why, true);  // blended
      if (!id) REXLOG_INFO("cod3: ngl side: rain texture {:08X} not drawn: {}", r.texture[1] & 0x1FFFF000, why);
      texture = g_rain_textures.emplace(texture_key, id).first;
    }
    if (!texture->second) continue;
    static std::atomic<int> density_shown{0};
    if (r.density != 0.0f && density_shown.fetch_add(1, std::memory_order_relaxed) < 2) {
      REXLOG_INFO("cod3: ngl side: rain with density {:.4g}, which isn't drawn natively", r.density);
    }
    const float fall = r.offset - std::floor(r.offset);  // the texture repeats every 1 in v
    // rn16d: the game's fade by the scene's depth, as ngl_vk's soft depth (see RainDraw); only with a backward depth
    // (turned over here) and a fade that grows toward the far
    // rn17: and the rain's own depth (ngl_vk's: 0 near to 1 far) where the game's fade, by the scene's depth alone,
    // and ngl_vk's, by how far the scene is behind the rain, are the same: 1 + DepthRange.y / DepthRange.x
    const float own_depth = r.depth_range < 0.0f ? 1.0f + r.depth_offset / r.depth_range : 0.0f;
    const bool soft = r.reversed && std::isfinite(r.depth_range) && r.depth_range < 0.0f &&
                      std::isfinite(own_depth) && own_depth > 0.0f && own_depth < 1.0f;
    std::vector<float> corners(static_cast<size_t>(count) * NGL_VK_CORNER_FLOATS, 0.0f);
    for (const Look& look : kLooks) {
      for (uint32_t c = 0; c < count; ++c) {
        const float* in = &r.corners[static_cast<size_t>(4) * c];
        float* out = &corners[static_cast<size_t>(c) * NGL_VK_CORNER_FLOATS];
        out[0] = kRainRadius * in[0];
        out[1] = kRainRadius * in[1];
        out[2] = kRainHeight * (in[2] - 0.5f);
        const float around = in[3] * r.thickness, slant = in[2] * r.cross;
        out[3] = look.scale * (look.set == 0 ? around + slant : around - slant);
        out[4] = look.scale * in[2] * r.stretch + fall;
        out[5] = soft ? -r.depth_range : out[3];  // rn16d: the soft depth's scale, in the lightmap u's place
        out[6] = out[4];
        const float alpha = std::min(std::max(2.0f * r.color * (1.0f - in[2]), 0.0f), 1.0f);
        const uint32_t color = 0x00FFFFFFu | static_cast<uint32_t>(alpha * 255.0f + 0.5f) << 24;  // white
        std::memcpy(&out[7], &color, 4);
      }
      ngl_vk_mesh_draw d{};
      d.mesh = mesh;
      d.corners = ngl_vk_frame_corners(corners.data(), count);
      if (!d.corners) break;  // the frame's room is used up
      d.texture = texture->second;
      d.shading = NGL_VK_SHADE_VERTEX_COLOR;
      d.light_once = 1;
      d.alpha = NGL_VK_ALPHA_BLEND;
      d.blend = NGL_VK_BLEND_ALPHA;
      d.soft_depth = soft ? 1 : 0;
      d.world[0] = d.world[5] = d.world[10] = d.world[15] = 1.0f;
      for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) d.view_projection[4 * row + column] = r.columns[4 * column + row];
        if (r.reversed) {  // a backward depth turned over (w - z), as EffectDraws does
          d.view_projection[4 * row + 2] = d.view_projection[4 * row + 3] - d.view_projection[4 * row + 2];
        }
        if (soft) d.view_projection[4 * row + 2] = own_depth * d.view_projection[4 * row + 3];  // rn17: z = own w
      }
      draws.push_back(d);
    }
  }
  if (!g_quiet && !rain.empty()) {
    REXLOG_INFO("cod3: ngl side: frame {}: rain: {} draws of {} kept", frame, draws.size(), rain.size());
  }
  return draws;
}
#endif

std::atomic<uint64_t> g_worker_wait_ns{0};  // side212: how long the game's main thread waited for the worker

#if defined(COD3_NGL_VK)
// side230 [acct2-bro] 2026-11-08: forget every ngl_vk texture and mesh and empty every cache that holds their numbers;
// what the next frame needs is made again from the game's memory. side228's block, moved here so a new level can use
// it too (ForgetIfNewLevel). On LiveFrame's thread, like every cache here.
void ForgetEverything() {
  ngl_vk_forget();
  g_textures.clear();
  g_cube_textures.clear();  // [rexglue-native] rn34
  g_d3d_textures.clear();
  g_cut_out.clear();
  g_meshes.clear();
  g_second_meshes.clear();
  g_meshes_full = false;
  g_tree_meshes.clear();
  g_sky_meshes.clear();
  g_sky_corners.clear();
  g_sky_indices.clear();
  g_sky_textures.clear();
  g_effect_textures.clear();
  g_effect_mesh = 0;
  g_rain_meshes.clear();  // rn16
  g_rain_textures.clear();
  g_sun = Sun();  // rn18: a level without a sun (The Mace) doesn't keep the one before's
  ++g_forgets;
}
#endif

// side228 [acct2-bro] 2026-11-08: with cod3_native_forget_level, before a frame is read: when ngl_vk is nearly out
// of room, forget every ngl_vk texture and mesh and empty every cache that holds their numbers; what the frame needs is
// made again from the game's memory (one slow frame, as at a level's start). Also a line every 600 frames with how many
// textures and meshes were made again at the same place (a new level's, or one the game keeps changing: watch for those).
// On LiveFrame's thread, like every cache here.
void ForgetIfFull() {
#if defined(COD3_NGL_VK)
  if (!REXCVAR_GET(cod3_native_forget_level)) return;
  static uint64_t checks = 0;
  uint32_t textures = 0, texture_room = 0, corners = 0, corner_room = 0;
  ngl_vk_room(&textures, &texture_room, &corners, &corner_room);
  if (++checks % 600 == 0 && (g_textures_made_again || g_meshes_made_again)) {
    REXLOG_INFO("cod3: ngl side: made again at the same place: {} textures, {} meshes; ngl_vk holds {} of {} "
                "textures, {} of {} corners",
                g_textures_made_again, g_meshes_made_again, textures, texture_room, corners, corner_room);
    g_textures_made_again = g_meshes_made_again = 0;
  }
  const bool full = g_meshes_full || textures + 128 > texture_room || corners > corner_room / 10 * 9;
  if (!full) return;
  REXLOG_INFO("cod3: ngl side: ngl_vk nearly full ({} of {} textures, {} of {} corners{}): every native texture and "
              "mesh is forgotten and made again as needed",
              textures, texture_room, corners, corner_room, g_meshes_full ? ", a mesh didn't fit" : "");
  ForgetEverything();
#endif
}

// side230 [acct2-bro] 2026-11-08: with cod3_native_forget_level, the first native frame after a level loads forgets the
// level before, so a long session holds one level's textures and meshes, not every level's (f232: after mp_merv and
// mp_fuel both stayed). One slow frame, as at any level's start. On LiveFrame's thread, like every cache here.
void ForgetIfNewLevel() {
#if defined(COD3_NGL_VK)
  if (!REXCVAR_GET(cod3_native_forget_level)) return;
  static uint32_t loads_seen = 0;
  const uint32_t loads = g_native_level_loads.load(std::memory_order_relaxed);
  if (loads == loads_seen) return;
  loads_seen = loads;
  uint32_t textures = 0, texture_room = 0, corners = 0, corner_room = 0;
  ngl_vk_room(&textures, &texture_room, &corners, &corner_room);
  REXLOG_INFO("cod3: ngl side: a new level: every native texture and mesh of the level before is forgotten "
              "(ngl_vk held {} of {} textures, {} of {} corners)",
              textures, texture_room, corners, corner_room);
  ForgetEverything();
#endif
}

// side119: a live frame drawn natively, timed (a line every kDumpEvery frames drawn). side212: on the game's main
// thread, or on the worker with cod3_native_worker; one frame at a time either way, so its counters need no lock.
void LiveFrame(uint8_t* base, uint32_t root, uint64_t frame) {
  ForgetIfNewLevel();  // side230
  ForgetIfFull();  // side228
#if defined(COD3_NGL_VK)
  {  // [rexglue-native] 2026-11-09: cod3_native_batched_textures as it is set now (it can change while the game runs);
     // only once ngl_vk has started (g_state 1: Dump starts it on the first frame; rn3 asked before that, was refused
     // for want of a device, and never asked again, so nothing was batched)
    static int asked = -1;
    const int want = REXCVAR_GET(cod3_native_batched_textures) ? 1 : 0;
    if (g_state == 1 && want != asked) {
      asked = want;
      const bool on = ngl_vk_set_batched_textures(want) == 0 && want;
      REXLOG_INFO("cod3: ngl side: new textures go to the graphics card {}", on ? "with the frame (batched)"
                  : want ? "one at a time (no room for the batch's staging buffer)" : "one at a time");
    }
  }
#endif
  static uint64_t live_frames = 0, live_since = 0;
  static double live_total_ms = 0.0, live_max_ms = 0.0;
#if defined(COD3_NGL_VK)
  // [rexglue-native] 2026-11-09: what the frames made new, summed between lines, and the slow frames' lines
  static FrameMade made_since;
  static int slow_lines = 0;
  g_frame_made = FrameMade{};
#endif
  const auto start = std::chrono::steady_clock::now();
  static auto line_at = start;  // side124: the game's frame rate between lines (a list send a frame)
  Dump(base, root, frame, false);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  live_total_ms += ms;
  live_max_ms = std::max(live_max_ms, ms);
  ++live_since;
#if defined(COD3_NGL_VK)
  made_since.textures += g_frame_made.textures;
  made_since.meshes += g_frame_made.meshes;
  made_since.texture_ms += g_frame_made.texture_ms;
  made_since.handed_ms += g_frame_made.handed_ms;
  if (ms > 50.0 && slow_lines < 60) {  // [rexglue-native] 2026-11-09: what a long native frame was spent on
    ++slow_lines;
    REXLOG_INFO("cod3: ngl side: live: frame {} took {:.1f} ms natively: {} new textures ({:.1f} ms, {:.1f} of it "
                "handing them to ngl_vk), {} new meshes{}",
                frame, ms, g_frame_made.textures, g_frame_made.texture_ms, g_frame_made.handed_ms,
                g_frame_made.meshes, slow_lines == 60 ? " (no more of these lines)" : "");
  }
#endif
  // a line every kDumpEvery frames drawn, and early ones (side122: a slow run never reached the first)
  if (++live_frames % kDumpEvery == 0 || live_frames == 30 || live_frames == 120) {
    const double seconds = std::chrono::duration<double>(start - line_at).count();
    line_at = start;
    REXLOG_INFO("cod3: ngl side: live: {} frames drawn natively so far; the last {} took {:.2f} ms on average, {:.2f} "
                "ms at most (frame {}); the game at {:.1f} frames a second", live_frames, live_since,
                live_total_ms / live_since, live_max_ms, frame, seconds > 0.0 ? live_since / seconds : 0.0);
    const double n = static_cast<double>(live_since);
    const double waited_ms = static_cast<double>(g_worker_wait_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
    double gpu_wait_ms = 0.0;  // side219: of the drawing, the time ngl_vk waited for the GPU's last frame
    double staged_mb = 0.0;    // side220: corners and indices copied to the GPU, and full halves waited for
    uint64_t staging_flushes = 0;
#if defined(COD3_NGL_VK)
    gpu_wait_ms = static_cast<double>(ngl_vk_take_gpu_wait(nullptr)) / 1e6;
    staged_mb = static_cast<double>(ngl_vk_take_staging(&staging_flushes)) / (1024.0 * 1024.0);
#endif
    REXLOG_INFO("cod3: ngl side: live: a frame's parts in ms: the scene walk {:.2f}, sky {:.2f}, solids {:.2f}, "
                "effects {:.2f}, shadow and grass {:.2f}, quads {:.2f}, drawing {:.2f} (waiting for the GPU {:.2f}), "
                "hand-over {:.2f}; the game's thread waited {:.2f} for the worker; {:.2f} MB a frame staged for the GPU "
                "({} full halves)",
                g_parts.scene / n, g_parts.sky / n, g_parts.solids / n, g_parts.effects / n,
                g_parts.shadow_and_grass / n, g_parts.quads / n, g_parts.drawing / n, gpu_wait_ms / n,
                g_parts.hand_over / n, waited_ms / n, staged_mb / n, staging_flushes);
#if defined(COD3_NGL_VK)
    {  // side229 [acct2-bro] 2026-11-08: f231 (cod3mp, one process) saw the solids' time grow 11 % in 80 s on mp_merv
       // and stay up after a level change while the data staged started over: does the scene or a cache grow?
      uint32_t textures = 0, texture_room = 0, corners = 0, corner_room = 0;
      ngl_vk_room(&textures, &texture_room, &corners, &corner_room);
      REXLOG_INFO("cod3: ngl side: live: a frame on average: {:.0f} solids in the scene, {:.0f} solid draws, {:.0f} "
                  "draws in all; cached: {} textures ({} Direct3D), {} meshes ({} second layers, {} tree parts, {} "
                  "sky), {} effect textures; ngl_vk holds {} of {} textures, {} of {} corners",
                  g_parts.scene_solids / n, g_parts.solid_draws / n, g_parts.all_draws / n, g_textures.size(),
                  g_d3d_textures.size(), g_meshes.size(), g_second_meshes.size(), g_tree_meshes.size(),
                  g_sky_meshes.size(), g_effect_textures.size(), textures, texture_room, corners, corner_room);
    }
    {  // [rexglue-native] 2026-11-09: the new textures and meshes since the last line, and how the textures went in
      uint64_t texture_flushes = 0;
      const uint64_t batched = ngl_vk_take_texture_uploads(&texture_flushes);
      REXLOG_INFO("cod3: ngl side: live: made since the last line: {} textures ({:.1f} ms, {:.1f} of it handing them "
                  "to ngl_vk; {} batched, {} full batches waited for), {} meshes",
                  made_since.textures, made_since.texture_ms, made_since.handed_ms, batched, texture_flushes,
                  made_since.meshes);
      made_since = FrameMade{};
    }
#endif
    g_parts = FrameParts{};
    live_total_ms = live_max_ms = 0.0;
    live_since = 0;
#if defined(COD3_NGL_VK)
    if (g_feed.handed || g_feed.dropped) {  // side121: the window feed (cod3_native_vulkan)
      const auto [paint_ms, paints] = g_painter->TakeTimes();
      REXLOG_INFO("cod3: ngl side: live: in the window: {} handed over ({:.2f} ms each on the drawing thread), {} "
                  "dropped while the last was painted; painting took {:.2f} ms on average",
                  g_feed.handed, g_feed.handed ? g_feed.show_ms / g_feed.handed : 0.0, g_feed.dropped,
                  paints ? paint_ms / paints : 0.0);
      g_feed.handed = g_feed.dropped = 0;
      g_feed.show_ms = 0.0;
    }
#endif
  }
}

// side212: the worker (cod3_native_worker). The game's main thread posts a live frame and goes on; the worker draws it
// with LiveFrame. The main thread waits for it at the next list send, before anything else there, so at most one frame
// is in flight and the frame counters, g_list_sends and ngl_vk are only ever used by one thread at a time.
struct Worker {
  std::mutex mutex;
  std::condition_variable posted, finished;
  bool busy = false;      // a frame posted and not yet drawn
  bool stopping = false;  // the program is ending: no more frames
  uint8_t* base = nullptr;
  uint32_t root = 0;
  uint64_t frame = 0;
};
std::atomic<Worker*> g_worker{nullptr};

void WorkerMain(Worker* w) {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), "ngl worker");  // as the thread profiler and cod3_profile name it
  // side212b: a thread started from the game's main thread takes its affinity, and the SDK pins each XThread to one
  // CPU (xthread.cpp: 1 << its index), so the worker shared the main thread's CPU (side212: 62-89 fps). It may run on
  // any CPU but the first six, which the game's six XThreads hold (on any CPU at all on a PC with eight or fewer).
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  const long count = std::min<long>(sysconf(_SC_NPROCESSORS_ONLN), CPU_SETSIZE);
  for (long i = count > 8 ? 6 : 0; i < count; ++i) CPU_SET(i, &cpus);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) != 0)
    REXLOG_WARN("cod3: ngl side: the worker keeps the game's main thread's CPU (pthread_setaffinity_np failed)");
#elif defined(_WIN32)
  // [rexglue-native] 2026-11-09: the same on Windows. SetThreadDescription (Windows 10 1607 on) is looked up rather
  // than imported, so the program still starts where it is missing. The CPUs as above, among the first 64 (one
  // processor group, as the SDK's own masks).
  using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
    const auto set = reinterpret_cast<SetDescription>(
        reinterpret_cast<void (*)()>(GetProcAddress(kernel, "SetThreadDescription")));
    if (set) set(GetCurrentThread(), L"ngl worker");
  }
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const DWORD count = std::min<DWORD>(system.dwNumberOfProcessors, 64);
  DWORD_PTR cpus = 0;
  for (DWORD i = count > 8 ? 6 : 0; i < count; ++i) cpus |= DWORD_PTR{1} << i;
  if (!cpus || !SetThreadAffinityMask(GetCurrentThread(), cpus))
    REXLOG_WARN("cod3: ngl side: the worker keeps the CPUs Windows gave it (SetThreadAffinityMask failed)");
#endif
  std::unique_lock<std::mutex> lock(w->mutex);
  for (;;) {
    w->posted.wait(lock, [w] { return w->busy; });
    if (!w->stopping) {
      uint8_t* const base = w->base;
      const uint32_t root = w->root;
      const uint64_t frame = w->frame;
      lock.unlock();
      LiveFrame(base, root, frame);
      lock.lock();
    }
    w->busy = false;
    w->finished.notify_all();
  }
}

// Waits until the worker has no frame (the main thread's side; at once when there is no worker)
void WaitForWorker() {
  Worker* const w = g_worker.load(std::memory_order_acquire);
  if (!w) return;
  std::unique_lock<std::mutex> lock(w->mutex);
  if (!w->busy) return;
  const auto start = std::chrono::steady_clock::now();
  w->finished.wait(lock, [w] { return !w->busy; });
  g_worker_wait_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now() - start)
                                                       .count()),
                             std::memory_order_relaxed);
}

// At exit (before ngl_side's own globals go): the frame being drawn finishes, and no other starts
void StopWorker() {
  Worker* const w = g_worker.load(std::memory_order_acquire);
  if (!w) return;
  std::unique_lock<std::mutex> lock(w->mutex);
  w->stopping = true;
  w->finished.wait(lock, [w] { return !w->busy; });
}

void PostToWorker(uint8_t* base, uint32_t root, uint64_t frame) {
  Worker* w = g_worker.load(std::memory_order_acquire);
  if (!w) {  // made once on the main thread, never freed: the thread waits for frames until the program ends
    w = new Worker();
    std::thread(WorkerMain, w).detach();
    std::atexit(StopWorker);
    g_worker.store(w, std::memory_order_release);
    REXLOG_INFO("cod3: ngl side: the native frames are read and drawn on their own thread (cod3_native_worker)");
  }
  std::lock_guard<std::mutex> lock(w->mutex);
  if (w->stopping) return;
  w->base = base;
  w->root = root;
  w->frame = frame;
  w->busy = true;
  w->posted.notify_one();
}

#if defined(COD3_NGL_VK)
// side215: cod3_debug_host_draws. The host backend calls Draw on the GPU command thread for each of the game's
// draws (resolves aside) with the registers as the game set them; only that thread touches the counts.
namespace host_draws {

constexpr uint32_t kColorInfo = 0x2001;    // RB_COLOR_INFO: 16-19 format, 20-25 exponent bias (signed)
constexpr uint32_t kColorMask = 0x2104;    // RB_COLOR_MASK: 0-3 render target 0's channels
constexpr uint32_t kBlend0 = 0x2201;       // RB_BLENDCONTROL0
constexpr uint32_t kModeControl = 0x2208;  // RB_MODECONTROL: 0-2 EDRAM mode
constexpr uint64_t kSkyCloudsShader = 0x24CFBA2A030227A9ull;  // the sky's cloud layers' vertex shader (side194)

struct Key {
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;
  uint32_t target = 0;  // render target 0's format and exponent bias (RB_COLOR_INFO 16-25), the EDRAM mode in 0-2,
                        // 1 << 31 when it writes no color
  uint32_t blend = 0;
  bool operator==(const Key& o) const {
    return vertex_shader == o.vertex_shader && pixel_shader == o.pixel_shader && target == o.target &&
           blend == o.blend;
  }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    return size_t(k.vertex_shader * 0x9E3779B97F4A7C15ull ^ k.pixel_shader ^ (uint64_t(k.target) << 32 | k.blend));
  }
};

const uint32_t* g_registers = nullptr;
uint32_t g_register_count = 0;
std::unordered_map<Key, uint64_t, KeyHash> g_counts;
uint64_t g_draws = 0;
std::chrono::steady_clock::time_point g_next_log;

int ExpBias(uint32_t target) { return int(int32_t(target << 6) >> 26); }  // bits 20-25, signed
uint32_t Format(uint32_t target) { return (target >> 16) & 0xF; }

std::string Describe(const Key& k, uint64_t count) {
  char text[160];
  std::snprintf(text, sizeof text, "vs %016llX ps %016llX %s format %u bias %+d blend %08X: %llu",
                (unsigned long long)k.vertex_shader, (unsigned long long)k.pixel_shader,
                (k.target >> 31) ? "no color," : "color", Format(k.target), ExpBias(k.target), k.blend,
                (unsigned long long)count);
  return text;
}

void Log() {
  std::map<int, uint64_t> by_bias;
  std::vector<std::pair<Key, uint64_t>> biased, sky;
  for (const auto& [key, count] : g_counts) {
    if (key.target >> 31) continue;
    by_bias[ExpBias(key.target)] += count;
    if (ExpBias(key.target) != 0) biased.push_back({key, count});
    if (key.vertex_shader == kSkyCloudsShader) sky.push_back({key, count});
  }
  std::string biases;
  for (const auto& [bias, count] : by_bias) {
    char item[48];
    std::snprintf(item, sizeof item, " %+d: %llu", bias, (unsigned long long)count);
    biases += item;
  }
  REXLOG_INFO("cod3: host draws: {} of the game's draws so far, {} kinds; color draws by exponent bias:{}", g_draws,
              g_counts.size(), biases.empty() ? " none" : biases);
  auto by_count = [](const auto& a, const auto& b) { return a.second > b.second; };
  std::sort(biased.begin(), biased.end(), by_count);
  for (size_t i = 0; i < biased.size() && i < 24; ++i)
    REXLOG_INFO("cod3: host draws: biased {}", Describe(biased[i].first, biased[i].second));
  for (const auto& [key, count] : sky) REXLOG_INFO("cod3: host draws: sky clouds {}", Describe(key, count));
  if (biased.empty()) {  // no bias anywhere: the render targets the color draws use, to see what else differs
    std::map<uint32_t, uint64_t> targets;
    for (const auto& [key, count] : g_counts)
      if (!(key.target >> 31)) targets[key.target & 0x03FF0007u] += count;
    std::string text;
    for (const auto& [target, count] : targets) {
      char item[64];
      std::snprintf(item, sizeof item, " (format %u mode %u: %llu)", Format(target), target & 7,
                    (unsigned long long)count);
      text += item;
    }
    REXLOG_INFO("cod3: host draws: color draws by render target 0:{}", text);
  }
}

int32_t Start(void*, const RexHostGpu* gpu) {
  if (gpu->struct_size < offsetof(RexHostGpu, register_count) + sizeof(uint32_t) || !gpu->registers ||
      gpu->register_count <= kModeControl)
    return 0;
  g_registers = gpu->registers;
  g_register_count = gpu->register_count;
  g_next_log = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  return 1;
}

// [rexglue-native] 2026-11-09 rn22: the constants and fetch constants the GPU really has at the soldiers' and the
// rifle's draws. Their effect parameters go into the command buffer, not into Direct3D's shadow of the constants, so
// ProbeModelShader read 0 from c16 up (rn20), and the native side doesn't know the light those shaders were given.
// The register file holds 512 float4 ALU constants from 0x4000 (the vertex shader's c0-c255, then the pixel
// shader's) and 32 six-dword fetch constants from 0x4800. Each watched pixel shader's first kWatchedLogged draws,
// logged with its light constants and how its textures are fetched (base and format, signs, exponent).
constexpr uint32_t kPixelConstants = 0x4000 + 256 * 4;
constexpr uint32_t kFetchConstants = 0x4800;
constexpr int kWatchedLogged = 40;  // rn23: 40 (rn22: 4), to pair them with the CPU side's draws by their key
struct Watched {
  uint64_t pixel_shader;
  const char* what;
  int first, last;  // the pixel constants logged
};
constexpr Watched kWatched[] = {
    {0x3BA30872B3955C23ull, "fabric (the uniforms; c17 LIGHT_COLOR1, c18-c26 irradiance, c27 PROJCOLOR)", 16, 27},
    {0xCFC45995D1294CCEull, "Springfield (c18 LIGHT_COLOR1, c19-c27 irradiance, c28 PROJCOLOR, c29-c32 material)", 16,
     32},
    {0xCDBAC6026B525050ull, "gun and arms (bumped)", 16, 28},
};
int g_watched_logged[std::size(kWatched)] = {};

void LogWatched(const RexHostDraw* draw) {
  if (g_register_count < kFetchConstants + 32 * 6) return;
  // rn25: from frame 2900, as the CPU side's lines (a debug read of the game's count; rn23: 1500), and only constants
  // not logged before (c17 up: c16, the eye, moves every frame), so the soldiers show beside the view model
  if (g_frames < 2900) return;
  static std::vector<uint64_t> seen[std::size(kWatched)];  // (the GPU thread only)
  for (size_t w = 0; w < std::size(kWatched); ++w) {
    if (draw->pixel_shader != kWatched[w].pixel_shader || g_watched_logged[w] >= kWatchedLogged) continue;
    uint64_t signature = 1469598103934665603ull;
    // rn27: the fabric's lines by their diffuse texture (tf5's base) and their rows (c18) instead, so the flickering
    // fire (c17) on the view model's sleeves doesn't take every line and each soldier's diffuse shows once
    const bool by_texture = kWatched[w].pixel_shader == 0x3BA30872B3955C23ull;
    const int from = by_texture ? 18 : kWatched[w].first + 1, to = by_texture ? 18 : kWatched[w].last;
    if (by_texture) signature = (signature ^ (g_registers[kFetchConstants + 6 * 5 + 1] & ~0xFFFu)) * 1099511628211ull;
    for (int c = from; c <= to; ++c) {
      for (int i = 0; i < 4; ++i) {
        float v;
        std::memcpy(&v, g_registers + kPixelConstants + 4 * c + i, sizeof v);
        const double scale = by_texture ? 100.0 : 1000.0;
        const double rounded = std::isfinite(v) ? std::round(std::clamp(double{v}, -1e9, 1e9) * scale) : 0.0;
        signature = (signature ^ static_cast<uint64_t>(static_cast<int64_t>(rounded))) * 1099511628211ull;
      }
    }
    if (std::find(seen[w].begin(), seen[w].end(), signature) != seen[w].end()) continue;
    seen[w].push_back(signature);
    ++g_watched_logged[w];
    std::string constants, fetches;
    char item[112];
    for (int c = kWatched[w].first; c <= kWatched[w].last; ++c) {
      float v[4];
      std::memcpy(v, g_registers + kPixelConstants + 4 * c, sizeof v);
      std::snprintf(item, sizeof item, " c%d=(%.4g %.4g %.4g %.4g)", c, v[0], v[1], v[2], v[3]);
      constants += item;
    }
    for (int slot = 1; slot <= 6; ++slot) {
      const uint32_t* f = g_registers + kFetchConstants + 6 * slot;
      const int raw = static_cast<int>((f[3] >> 13) & 0x3F);
      std::snprintf(item, sizeof item, " tf%d %08X/%u signs %02X exp %d", slot, f[1] & ~0xFFFu, f[1] & 0x3F,
                    (f[0] >> 2) & 0xFF, raw >= 32 ? raw - 64 : raw);
      fetches += item;
    }
    // rn23: the key the CPU side's "rn23 cpu draw" lines carry: the vertex constants' c0.w c1.w c2.w (the world
    // matrix's translation)
    float key[3];
    for (int i = 0; i < 3; ++i) std::memcpy(&key[i], g_registers + 0x4000 + 4 * i + 3, sizeof key[i]);
    REXLOG_INFO("cod3: host draws: {} (draw {}): key {:.4g} {:.4g} {:.4g}; pixel{}; fetch{}", kWatched[w].what, g_draws,
                key[0], key[1], key[2], constants, fetches);
  }
}

// [rexglue-native] 2026-11-09 rn28: the game's draws that read one of the diffuse textures the native side draws as
// fabric (g_fabric_diffuse), through any pixel shader but the fabric one: once per (pixel shader, texture), up to
// kFabricShadersLogged, with the shaders, which fetch slot holds it, the pixel constants c16-c31 and the first eight
// fetch slots' bases. From frame 2900 (debug only, host draws).
constexpr int kFabricShadersLogged = 24;
void LogFabricTextureDraws(const RexHostDraw* draw) {
  if (g_register_count < kFetchConstants + 32 * 6 || g_frames < 2900) return;
  if (draw->pixel_shader == 0x3BA30872B3955C23ull) return;
  static int logged = 0;  // (the GPU thread only)
  static std::vector<std::pair<uint64_t, uint32_t>> seen;
  if (logged >= kFabricShadersLogged) return;
  const uint32_t count = g_fabric_diffuse_count.load(std::memory_order_acquire);
  if (!count) return;
  for (int slot = 0; slot < 16; ++slot) {
    const uint32_t at = g_registers[kFetchConstants + 6 * slot + 1] & 0x1FFFF000u;
    if (!at) continue;
    bool kept = false;
    for (uint32_t i = 0; i < count && !kept; ++i) kept = g_fabric_diffuse[i].load(std::memory_order_relaxed) == at;
    if (!kept) continue;
    const std::pair<uint64_t, uint32_t> pair{draw->pixel_shader, at};
    if (std::find(seen.begin(), seen.end(), pair) != seen.end()) return;
    seen.push_back(pair);
    ++logged;
    std::string constants, fetches;
    char item[112];
    // rn29: and c252-c255, where the shader's own literal constants sit (rn21: the first 64 bytes of its block)
    for (int c : {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 252, 253, 254, 255}) {
      float v[4];
      std::memcpy(v, g_registers + kPixelConstants + 4 * c, sizeof v);
      std::snprintf(item, sizeof item, " c%d=(%.6g %.6g %.6g %.6g)", c, v[0], v[1], v[2], v[3]);
      constants += item;
    }
    for (int f = 0; f < 8; ++f) {
      std::snprintf(item, sizeof item, " tf%d %08X", f, g_registers[kFetchConstants + 6 * f + 1] & 0x1FFFF000u);
      fetches += item;
    }
    float key[3];
    for (int i = 0; i < 3; ++i) std::memcpy(&key[i], g_registers + 0x4000 + 4 * i + 3, sizeof key[i]);
    REXLOG_INFO("cod3: host draws: rn28 fabric diffuse {:08X} in tf{} of vs {:016X} ps {:016X} (draw {}): key {:.4g} "
                "{:.4g} {:.4g}; pixel{}; fetch{}", at, slot, draw->vertex_shader, draw->pixel_shader, g_draws, key[0],
                key[1], key[2], constants, fetches);
    return;
  }
}

// [rexglue-native] 2026-11-09 rn35: the game's draws of the watched color maps (cod3_debug_ngl_side_watch, kept by the
// native side in g_watched_diffuse): once per (pixel shader, texture), up to kWatchedShadersLogged, with the shaders,
// the fetch slot, the render target's format, exponent bias and blend, the pixel constants c16-c40 and c252-c255 (the
// shader's literals), and the first eight fetch slots' bases, formats and exponents. From frame 2900 (debug only, host
// draws).
constexpr int kWatchedShadersLogged = 32;
constexpr int kAfterWatchedLogged = 2, kAfterWatchedTimes = 4;  // rn41k (LogAfterWatched)
int g_after_watched = 0, g_after_watched_times = 0;              // (the GPU thread only)
void LogWatchedTextureDraws(const RexHostDraw* draw) {
  if (g_register_count < kFetchConstants + 32 * 6 || g_frames < 2900) return;
  static int logged = 0;  // (the GPU thread only)
  static std::vector<std::pair<uint64_t, uint32_t>> seen;
  if (logged >= kWatchedShadersLogged) return;
  const uint32_t count = g_watched_diffuse_count.load(std::memory_order_acquire);
  if (!count) return;
  for (int slot = 0; slot < 16; ++slot) {
    const uint32_t at = g_registers[kFetchConstants + 6 * slot + 1] & 0x1FFFF000u;
    if (!at) continue;
    bool kept = false;
    for (uint32_t i = 0; i < count && !kept; ++i) kept = g_watched_diffuse[i].load(std::memory_order_relaxed) == at;
    if (!kept) continue;
    const std::pair<uint64_t, uint32_t> pair{draw->pixel_shader, at};
    if (std::find(seen.begin(), seen.end(), pair) != seen.end()) return;
    seen.push_back(pair);
    ++logged;
    std::string constants, fetches;
    char item[112];
    for (int c = 16; c <= 255; c = c == 40 ? 252 : c + 1) {
      float v[4];
      std::memcpy(v, g_registers + kPixelConstants + 4 * c, sizeof v);
      std::snprintf(item, sizeof item, " c%d=(%.6g %.6g %.6g %.6g)", c, v[0], v[1], v[2], v[3]);
      constants += item;
    }
    for (int f = 0; f < 8; ++f) {
      const uint32_t* w = g_registers + kFetchConstants + 6 * f;
      const int raw = static_cast<int>((w[3] >> 13) & 0x3F);
      std::snprintf(item, sizeof item, " tf%d %08X/%u exp %d", f, w[1] & 0x1FFFF000u, w[1] & 0x3F,
                    raw >= 32 ? raw - 64 : raw);
      fetches += item;
    }
    float key[3];
    for (int i = 0; i < 3; ++i) std::memcpy(&key[i], g_registers + 0x4000 + 4 * i + 3, sizeof key[i]);
    REXLOG_INFO("cod3: host draws: rn35 watched {:08X} in tf{} of vs {:016X} ps {:016X} (draw {}): target format {} "
                "bias {:+d} blend {:08X}; key {:.4g} {:.4g} {:.4g}; pixel{}; fetch{}", at, slot, draw->vertex_shader,
                draw->pixel_shader, g_draws, Format(g_registers[kColorInfo]), ExpBias(g_registers[kColorInfo]),
                g_registers[kBlend0], key[0], key[1], key[2], constants, fetches);
    if (g_after_watched_times < kAfterWatchedTimes) {  // rn41k
      ++g_after_watched_times;
      g_after_watched = kAfterWatchedLogged;
    }
    return;
  }
}

// [rexglue-native] 2026-11-10 rn41k: the draws right after a watched one. rn41j: the game draws the lantern's glass
// with 5DD7AA099EE15C4F (its color map times the corners' colors, blended by the map's alpha), the first of its
// technique's two passes; 0x82143F70 draws such a technique's passes one after another, so the second pass is the next
// draw. kAfterWatchedLogged draws after each of the first kAfterWatchedTimes watched ones, with their shaders, blend,
// fetch slots, and vertex (c16-c31) and pixel (c16-c40, c252-c255) constants (debug only, host draws).
void LogAfterWatched(const RexHostDraw* draw) {
  if (g_after_watched <= 0 || g_register_count < kFetchConstants + 32 * 6) return;
  --g_after_watched;
  std::string vertex, pixel, fetches;
  char item[112];
  for (int c = 16; c <= 31; ++c) {
    float v[4];
    std::memcpy(v, g_registers + 0x4000 + 4 * c, sizeof v);
    std::snprintf(item, sizeof item, " c%d=(%.6g %.6g %.6g %.6g)", c, v[0], v[1], v[2], v[3]);
    vertex += item;
  }
  for (int c = 16; c <= 255; c = c == 40 ? 252 : c + 1) {
    float v[4];
    std::memcpy(v, g_registers + kPixelConstants + 4 * c, sizeof v);
    std::snprintf(item, sizeof item, " c%d=(%.6g %.6g %.6g %.6g)", c, v[0], v[1], v[2], v[3]);
    pixel += item;
  }
  for (int f = 0; f < 8; ++f) {
    const uint32_t* w = g_registers + kFetchConstants + 6 * f;
    const int raw = static_cast<int>((w[3] >> 13) & 0x3F);
    std::snprintf(item, sizeof item, " tf%d %08X/%u exp %d", f, w[1] & 0x1FFFF000u, w[1] & 0x3F,
                  raw >= 32 ? raw - 64 : raw);
    fetches += item;
  }
  REXLOG_INFO("cod3: host draws: rn41k after watched: vs {:016X} ps {:016X} (draw {}): target format {} bias {:+d} "
              "blend {:08X}; fetch{}; vertex{}; pixel{}", draw->vertex_shader, draw->pixel_shader, g_draws,
              Format(g_registers[kColorInfo]), ExpBias(g_registers[kColorInfo]), g_registers[kBlend0], fetches,
              vertex, pixel);
}

// [rexglue-native] 2026-11-10 rn41t: the game's leaf draws (St-Bert rn41s: the native leaves come out flat dark green,
// the game's lit yellow-green toward the sun). SpeedTree's leaves (sub_823F5028) are drawn with pixel shader
// 6189945E43D7FBC8: out = saturate(the leaf map x (the vertex shader's light x c16 g_fDiffuseScale + c17
// g_vMaterialAmbient)) x c22.w, cut out below c18 g_fTreeAlphaTestValue; its vertex shader lights each leaf from c58 /
// c59 g_vLightColor0 / 1 and c60 / c61 g_vLightDir0 / 1 (rn41s's saved shaders, named by tools' ps_names.py). The
// game sets these by its effect's handles, not in the device's copy, so they are read here at the GPU's draws: up to
// kLeafDrawsLogged draws whose values differ, from frame 1500, with the shaders, the leaf map's base, the pixel
// constants c16-c22 and c252-c255 and the vertex constants c40-c95 (rn41w: from c40, for c45 g_vMaterialDiffuse) and
// c252-c255 (debug only, host draws).
constexpr uint64_t kLeafPixelShader = 0x6189945E43D7FBC8ull;
constexpr int kLeafDrawsLogged = 8;
void LogLeafDraws(const RexHostDraw* draw) {
  if (draw->pixel_shader != kLeafPixelShader || g_register_count < kFetchConstants + 32 * 6 || g_frames < 1500 ||
      !REXCVAR_GET(cod3_debug_ngl_side))
    return;
  static int logged = 0;  // (the GPU thread only)
  static std::vector<uint64_t> seen;
  if (logged >= kLeafDrawsLogged) return;
  uint64_t signature = 1469598103934665603ull;  // the material's and the lights' values, as stored
  for (int c = 16; c <= 18; ++c) {
    for (int i = 0; i < 4; ++i) signature = (signature ^ g_registers[kPixelConstants + 4 * c + i]) * 1099511628211ull;
  }
  for (int c = 57; c <= 61; ++c) {
    for (int i = 0; i < 4; ++i) signature = (signature ^ g_registers[0x4000 + 4 * c + i]) * 1099511628211ull;
  }
  if (std::find(seen.begin(), seen.end(), signature) != seen.end()) return;
  seen.push_back(signature);
  ++logged;
  std::string vertex, pixel;
  char item[112];
  for (int c = 16; c <= 255; c = c == 22 ? 252 : c + 1) {
    float v[4];
    std::memcpy(v, g_registers + kPixelConstants + 4 * c, sizeof v);
    std::snprintf(item, sizeof item, " c%d=(%.6g %.6g %.6g %.6g)", c, v[0], v[1], v[2], v[3]);
    pixel += item;
  }
  for (int c = 40; c <= 255; c = c == 95 ? 252 : c + 1) {
    float v[4];
    std::memcpy(v, g_registers + 0x4000 + 4 * c, sizeof v);
    std::snprintf(item, sizeof item, " c%d=(%.6g %.6g %.6g %.6g)", c, v[0], v[1], v[2], v[3]);
    vertex += item;
  }
  const uint32_t* map = g_registers + kFetchConstants;  // the leaf map is tf0 (samCompositeLeafMap s0)
  REXLOG_INFO("cod3: host draws: rn41t leaves: vs {:016X} ps {:016X} (draw {}, frame {}): leaf map {:08X}/{}; "
              "blend {:08X}; pixel{}; vertex{}", draw->vertex_shader, draw->pixel_shader, g_draws, g_frames,
              map[1] & 0x1FFFF000u, map[1] & 0x3F, g_registers[kBlend0], pixel, vertex);
}

// [rexglue-native] 2026-11-10 rn42d: the game's effect draws (the quad batcher's vertex shaders: the plain pair's
// 6AB7A24B1D031539 and the soft pair's AACFBC52742E4628), to see which textures the GPU samples. rn42c: one flush's
// texture kept from Direct3D's copy of fetch constant 0 (KeepEffectDraw) was 0D84C000, a 16 x 16 opaque white frame
// with a dark middle (NGL's default texture?), drawn natively as the tank barrel's square smoke (owner 09:13), where
// the game's picture has soft haze. Once per distinct (shaders, fetch slots 0-3 bases), up to 48 lines, from frame 600:
// the shaders, the blend, fetch slots 0-3 (base, format, size) and pixel c0 (debug only, host draws).
constexpr uint64_t kEffectPlainVertexShader = 0x6AB7A24B1D031539ull;
constexpr uint64_t kEffectSoftVertexShader = 0xAACFBC52742E4628ull;
void LogEffectHostDraws(const RexHostDraw* draw) {
  if ((draw->vertex_shader != kEffectPlainVertexShader && draw->vertex_shader != kEffectSoftVertexShader) ||
      g_register_count < kFetchConstants + 32 * 6 || g_frames < 600 || !REXCVAR_GET(cod3_debug_ngl_side))
    return;
  static int logged = 0;  // (the GPU thread only)
  static std::vector<uint64_t> seen;
  if (logged >= 48) return;
  uint64_t signature = draw->vertex_shader * 31 + draw->pixel_shader;
  for (int f = 0; f < 4; ++f) signature = signature * 1000003 + (g_registers[kFetchConstants + 6 * f + 1] & 0x1FFFF000u);
  if (std::find(seen.begin(), seen.end(), signature) != seen.end()) return;
  seen.push_back(signature);
  ++logged;
  std::string fetches;
  char item[96];
  for (int f = 0; f < 4; ++f) {
    const uint32_t* w = g_registers + kFetchConstants + 6 * f;
    std::snprintf(item, sizeof item, " tf%d %08X/%u %ux%u", f, w[1] & 0x1FFFF000u, w[1] & 0x3F, (w[2] & 0x1FFF) + 1,
                  ((w[2] >> 13) & 0x1FFF) + 1);
    fetches += item;
  }
  float c0[4];
  std::memcpy(c0, g_registers + kPixelConstants, sizeof c0);
  REXLOG_INFO("cod3: host draws: rn42d effect draw: vs {:016X} ps {:016X} (draw {}, frame {}): blend {:08X};{}; pixel "
              "c0 ({:.4g} {:.4g} {:.4g} {:.4g})",
              draw->vertex_shader, draw->pixel_shader, g_draws, g_frames, g_registers[kBlend0], fetches, c0[0], c0[1],
              c0[2], c0[3]);
}

// [rexglue-native] 2026-11-10 rn42e/rn42f: which texture the GPU draws the native side's square flushes with. Both effect
// vertex shaders (rn9's dump: shader_AACFBC52742E4628 / 6AB7A24B1D031539.ucode.vert) move a corner to c25 (NAN_VALUE)
// when (c255.x > w) and ((c255.w > its alpha, and |x| or |y| > w x c255.z) != c255.y); rn42e read the literals,
// c255 = (200 0 1.5 0.05): a corner nearer than 200 that is faint (alpha under 0.05) and off the screen by half
// again. That hides no visible quad, so the shaders don't explain the squares. rn42e's 24 GPU draws with 0D84C000
// (frames 1965-1988) were all of one effect (c14 -0.000769 1.154), while the native side's 10-quad square flush is
// another (c14 -0.0005882 1.176). rn42f: once per distinct (fetch slot 0's base, index count, kind, shaders), up to 60
// lines from frame 600, the effect draws whose texture is 0D84C000 or whose c14.x is that flush's: the texture, the
// count, c14, c16, and the first corner (vertex fetch constant 95 at the draw's first index: place and color), to
// pair the GPU's draw of that flush with the native side's (KeepEffectDraw's rn42f lines; debug only, host draws).
float VertexConstant(int c, int i) {
  float v;
  std::memcpy(&v, g_registers + 0x4000 + 4 * c + i, sizeof v);
  return v;
}

// [rexglue-native] 2026-11-10 rn42m: the game's own effect draws on each frame (the alpha-only ones aside, as
// natively), logged on the frames cod3_debug_ngl_side_live_shots saves and the four after (the GPU runs behind), to set
// beside the native frames' effects lines: does the game draw the same flushes every frame, or alternate (rn42l, with
// cod3_native_effects_whole_list, drew 138 and 118 flushes on alternate frames)? A frame's effects end at their
// blend-back (VS 118071C8F97830EA), which the game draws once a frame after them.
constexpr uint64_t kEffectBlendBackVertexShader = 0x118071C8F97830EAull;
void CountEffectFrame(const RexHostDraw* draw) {
  static uint32_t draws = 0, quads = 0;  // (the GPU thread only)
  static std::map<uint32_t, uint32_t> textures;
  if (draw->vertex_shader == kEffectPlainVertexShader || draw->vertex_shader == kEffectSoftVertexShader) {
    if (!(g_registers[kColorMask] & 7u) || g_register_count < kFetchConstants + 32 * 6) return;
    ++draws;
    quads += draw->index_count / 6;
    ++textures[g_registers[kFetchConstants + 1] & 0x1FFFF000u];
    return;
  }
  if (draw->vertex_shader != kEffectBlendBackVertexShader) return;
  const uint64_t frame = g_frames;
  bool window = false;
  for (uint64_t back = 0; back <= 4 && !window; ++back) window = frame >= back && LiveShotFrame(frame - back);
  if (window && REXCVAR_GET(cod3_debug_ngl_side)) {
    std::string list;
    for (const auto& [address, count] : textures) {
      char item[24];
      std::snprintf(item, sizeof item, " %08X x%u", address, count);
      list += item;
    }
    REXLOG_INFO("cod3: host draws: rn42m GPU effects (native frame {}): {} draws, {} quads;{}", frame, draws, quads,
                list);
  }
  draws = quads = 0;
  textures.clear();
}

void LogEffectFlushes(const RexHostDraw* draw) {
  if ((draw->vertex_shader != kEffectPlainVertexShader && draw->vertex_shader != kEffectSoftVertexShader) ||
      g_register_count < kFetchConstants + 32 * 6 || g_frames < 600 || !REXCVAR_GET(cod3_debug_ngl_side))
    return;
  const uint32_t tf0 = g_registers[kFetchConstants + 1] & 0x1FFFF000u;
  const bool square_kind = std::fabs(VertexConstant(14, 0) + 0.0005882f) < 0.00002f;
  // rn42g: and, to compare, the first 8 other textures' draws from frame 1800 (once per texture)
  static int others = 0;  // (the GPU thread only)
  static std::vector<uint32_t> other_seen;
  const bool other = tf0 != 0x0D84C000u && !square_kind;
  if (other) {
    if (others >= 8 || g_frames < 1800 ||
        std::find(other_seen.begin(), other_seen.end(), tf0) != other_seen.end())
      return;
    other_seen.push_back(tf0);
    ++others;
  }
  static int logged = 0;
  static std::vector<uint64_t> seen;
  if (!other) {
    if (logged >= 60) return;
    const uint64_t key = (uint64_t{tf0} << 32) ^ (uint64_t{draw->index_count} << 4) ^ (square_kind ? 1u : 0u) ^
                         (draw->vertex_shader == kEffectSoftVertexShader ? 2u : 0u);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    ++logged;
  }
  // rn42g: where the draw goes. RB_SURFACE_INFO (0x2000: 0-13 the surface's pitch), RB_COLOR_INFO (0x2001: 0-11 the
  // EDRAM base, 16-19 the format), RB_DEPTH_INFO (0x2002: 0-11 the depth's EDRAM base), RB_COLOR_MASK (0x2104),
  // RB_DEPTHCONTROL (0x2200: bit 1 the depth test, 2 its write, 4-6 the function), PA_SC_WINDOW_SCISSOR_TL / BR
  // (0x2081 / 0x2082: x 0-14, y 16-30) and PA_CL_VPORT_XSCALE / YSCALE (0x210F / 0x2111, floats).
  auto real_register = [](uint32_t r) {
    float f;
    std::memcpy(&f, g_registers + r, sizeof f);
    return f;
  };
  char target[200];
  std::snprintf(target, sizeof target,
                "surface %08X color %08X depth %08X mask %X depthcontrol %08X scissor %u,%u-%u,%u viewport scale "
                "%.4g %.4g",
                g_registers[0x2000], g_registers[0x2001], g_registers[0x2002], g_registers[kColorMask] & 0xF,
                g_registers[0x2200], g_registers[0x2081] & 0x7FFF, (g_registers[0x2081] >> 16) & 0x7FFF,
                g_registers[0x2082] & 0x7FFF, (g_registers[0x2082] >> 16) & 0x7FFF, real_register(0x210F),
                real_register(0x2111));
  // The first corner: vertex fetch constant 95 (2 dwords: type 3 and the address in bytes; the size in dwords in the
  // second's bits 2-25, its endianness in bits 0-1), at the draw's first index (16-bit, as the batcher's buffer).
  char corner[160] = " -";
  uint8_t* physical = PhysicalBase();
  const uint32_t d0 = g_registers[kFetchConstants + 190], d1 = g_registers[kFetchConstants + 191];
  if (physical && (d0 & 3) == 3 && draw->indexed && draw->index_format == 0 && draw->index_guest_address &&
      draw->index_guest_address < 0x20000000u) {
    const uint8_t* index = physical + draw->index_guest_address;
    const uint32_t first =
        draw->index_endian ? (uint32_t{index[0]} << 8 | index[1]) : (uint32_t{index[1]} << 8 | index[0]);
    const uint32_t at = (d0 & 0x1FFFFFFCu) + 48 * first;
    if (uint64_t{12} * (first + 1) <= ((d1 >> 2) & 0xFFFFFFu) && at < 0x20000000u) {
      auto word = [&](uint32_t from) {
        uint32_t w;
        std::memcpy(&w, physical + from, 4);
        return (d1 & 3) == 2 ? __builtin_bswap32(w) : w;
      };
      float p[3];
      for (int k = 0; k < 3; ++k) {
        const uint32_t w = word(at + 4 * k);
        std::memcpy(&p[k], &w, 4);
      }
      std::snprintf(corner, sizeof corner, " index %u: (%.1f %.1f %.1f) color %08X", first, p[0], p[1], p[2],
                    word(at + 12));
    }
  }
  REXLOG_INFO("cod3: host draws: rn42f effect flush{}: vs {:016X} (draw {}, frame {}): tf0 {:08X} {}x{}, count {}, c14 "
              "({:.4g} {:.4g}), c16 ({:.3g} {:.3g} {:.3g} {:.3g}), blend {:08X}; {}; first corner{}",
              other ? " (other)" : "", draw->vertex_shader, g_draws, g_frames, tf0,
              (g_registers[kFetchConstants + 2] & 0x1FFF) + 1, ((g_registers[kFetchConstants + 2] >> 13) & 0x1FFF) + 1,
              draw->index_count, VertexConstant(14, 0), VertexConstant(14, 1), VertexConstant(16, 0),
              VertexConstant(16, 1), VertexConstant(16, 2), VertexConstant(16, 3), g_registers[kBlend0], target,
              corner);
}

// [rexglue-native] 2026-11-10 rn42i: how the game brings its quarter-size effects back into the picture (rn42g: its
// smoke, dust and fire go to a 640 x 360 target, surface 0A000280 color base 0x2D0, all four channels; the tank smoke's
// framed quads write only the full-size frame's alpha). On frames 1800, 1950 and 2100, from the first effect draw into
// the quarter-size target, the next kTraceEvents draws and resolves in order: a draw's shaders, primitive and count,
// fetch slots 0-3 (base and size), the surface and color target, the mask, the blend and the depth control; a resolve's
// RB_COPY_CONTROL (0x2318), destination base, pitch and info (0x2319-0x231B), and the surface and color it copies from
// (debug only, host draws).
constexpr int kTraceEvents = 140;
int g_trace_left = 0;            // (the GPU thread only)
uint64_t g_trace_frame = 0;
std::vector<uint64_t> g_traced;  // the frames traced

std::string TraceTarget() {
  char text[160];
  std::snprintf(text, sizeof text, "surface %08X color %08X depth %08X mask %X blend %08X depthcontrol %08X",
                g_registers[0x2000], g_registers[0x2001], g_registers[0x2002], g_registers[kColorMask] & 0xF,
                g_registers[kBlend0], g_registers[0x2200]);
  return text;
}

void TraceDraw(const RexHostDraw* draw) {
  if (g_register_count < kFetchConstants + 32 * 6 || !REXCVAR_GET(cod3_debug_ngl_side)) return;
  // (the game's frame count moves on while the GPU thread is still on the frame before, so a trace runs by its count
  // of events, not until the count changes)
  const bool effect = draw->vertex_shader == kEffectPlainVertexShader || draw->vertex_shader == kEffectSoftVertexShader;
  constexpr uint64_t kFrom[3] = {1800, 1950, 2100};  // each trace starts at the first such draw from its frame on
  if (g_trace_left == 0 && effect && g_traced.size() < 3 && g_frames >= kFrom[g_traced.size()] &&
      g_registers[0x2000] == 0x0A000280u) {
    g_traced.push_back(g_frames);
    g_trace_frame = g_frames;
    g_trace_left = kTraceEvents;
  }
  if (g_trace_left <= 0) return;
  --g_trace_left;
  std::string fetches;
  char item[64];
  for (int f = 0; f < 4; ++f) {
    const uint32_t* w = g_registers + kFetchConstants + 6 * f;
    if ((w[0] & 3) != 2) continue;
    std::snprintf(item, sizeof item, " tf%d %08X/%u %ux%u", f, w[1] & 0x1FFFF000u, w[1] & 0x3F, (w[2] & 0x1FFF) + 1,
                  ((w[2] >> 13) & 0x1FFF) + 1);
    fetches += item;
  }
  REXLOG_INFO("cod3: host draws: rn42i trace (frame {}, {} left): draw vs {:016X} ps {:016X} primitive {} count {};{}; "
              "{}",
              g_trace_frame, g_trace_left, draw->vertex_shader, draw->pixel_shader, draw->primitive_type,
              draw->index_count, fetches.empty() ? " no textures" : fetches, TraceTarget());
}

void Resolve(void*) {
  if (g_trace_left <= 0 || g_register_count < kFetchConstants + 32 * 6) return;
  --g_trace_left;
  REXLOG_INFO("cod3: host draws: rn42i trace (frame {}, {} left): resolve copy control {:08X} dest {:08X} pitch {:08X} "
              "info {:08X}; from {}",
              g_trace_frame, g_trace_left, g_registers[0x2318], g_registers[0x2319], g_registers[0x231A],
              g_registers[0x231B], TraceTarget());
}

void Draw(void*, const RexHostDraw* draw) {
  ++g_draws;
  TraceDraw(draw);  // [rexglue-native] rn42i
  LogWatched(draw);
  LogFabricTextureDraws(draw);  // rn28
  LogAfterWatched(draw);         // [rexglue-native] rn41k: before, so the watched draw itself isn't one of them
  LogWatchedTextureDraws(draw);  // [rexglue-native] rn35
  LogLeafDraws(draw);            // [rexglue-native] rn41t
  LogEffectHostDraws(draw);      // [rexglue-native] rn42d
  LogEffectFlushes(draw);        // [rexglue-native] rn42f (rn42e)
  CountEffectFrame(draw);        // [rexglue-native] rn42m
  Key key;
  key.vertex_shader = draw->vertex_shader;
  key.pixel_shader = draw->pixel_shader;
  key.target = (g_registers[kColorInfo] & 0x03FF0000u) | (g_registers[kModeControl] & 7u);
  if (!(g_registers[kColorMask] & 0xF)) key.target |= 1u << 31;
  key.blend = g_registers[kBlend0];
  ++g_counts[key];
  if ((g_draws & 255) == 0 && std::chrono::steady_clock::now() >= g_next_log) {
    g_next_log += std::chrono::seconds(10);
    Log();
  }
}

const RexHostRenderer* Table() {
  static const RexHostRenderer table = [] {
    RexHostRenderer t = {};
    t.struct_size = uint32_t(sizeof(t));
    t.abi_version = REX_HOST_RENDERER_ABI_VERSION;
    t.name = "cod3_debug_host_draws";
    t.start = Start;
    t.draw = Draw;
    t.resolve = Resolve;  // [rexglue-native] rn42i (debug: only logs while a trace runs)
    return t;
  }();
  return &table;
}

}  // namespace host_draws
#endif

}  // namespace

void OnListSend(uint8_t* base) {
  WaitForWorker();  // side212: the last frame is drawn before this one counts as sent
  g_list_sends.fetch_add(1, std::memory_order_relaxed);
  const bool debug = REXCVAR_GET(cod3_debug_ngl_side), live = Live();
  if (!live && (!debug || g_dumps >= DumpCount())) return;
  // side212: the root scene as it is now; the game's nglListSend clears it as soon as this returns
  const uint32_t root = g::ReadU32(base, kRootScene);
  // rn42m: cod3_debug_ngl_side_shot_burst frames in a row at each moment (1: the moment alone, as before)
  if (++g_frames % DumpEvery() < DumpBurst() && g_frames >= DumpEvery() && debug && g_dumps < DumpCount()) {
    ++g_dumps;
    Dump(base, root, g_frames);
    if (g_dumps == 3 || g_dumps == 6) ReadNextDraws();
    return;
  }
  if (!live) return;
  if (REXCVAR_GET(cod3_native_worker)) {
    PostToWorker(base, root, g_frames);
    return;
  }
  LiveFrame(base, root, g_frames);
}

bool StartNativeVulkan(rex::RuntimeConfig& config) {
  if (!REXCVAR_GET(cod3_native_vulkan)) return false;
#if defined(COD3_NGL_VK)
  if (config.graphics || config.gpu_plugin.empty()) {
    REXLOG_WARN("cod3: native Vulkan: {}; the emulated GPU draws",
                config.graphics ? "the graphics already started" : "no GPU plugin (cod3_no_gpu)");
    return false;
  }
#if defined(_WIN32)
  // [rexglue-native] 2026-11-09: native mode draws with Vulkan (the host backend's window is always Vulkan's, and ngl_vk
  // draws on its device), so with Direct3D 12 chosen it stays off and the game keeps the API it was given.
  if (REXCVAR_GET(cod3_graphics_api) == "d3d12") {
    REXLOG_WARN("cod3: native Vulkan: needs the Vulkan graphics API, and cod3_graphics_api is d3d12 (F2 > Video); the "
                "emulated GPU draws, with Direct3D 12");
    return false;
  }
#endif
  // [rexglue-native] 2026-11-09: ngl_vk opens the system's Vulkan loader itself; without one native mode stays off
  // (and on Windows "auto" goes on to Direct3D 12 when Vulkan can't start)
  if (!ngl_vk_loader()) {
    REXLOG_WARN("cod3: native Vulkan: no Vulkan loader on this PC (ngl_vk can't start); the emulated GPU draws");
    return false;
  }
  // The host backend reads and answers the game's GPU commands (registers, fences, interrupts, swaps) as the
  // emulated GPU does, but draws nothing and hands the draws to no one: the picture is ngl_vk's (Dump, Show).
  // side215: cod3_debug_host_draws hands the backend a table that only counts the game's draws (host_draws).
  config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "host",
                                               REXCVAR_GET(cod3_debug_host_draws) ? host_draws::Table() : nullptr);
  if (!config.graphics) {
    REXLOG_ERROR("cod3: native Vulkan: the GPU plugin's host backend didn't load; the emulated GPU draws");
    return false;
  }
  g_native = true;
  // The coronas' depth test and the aim's focus read the emulated GPU's depth back on the CPU. Without it there is
  // no depth to read, so they trace the world instead (claude-9b's cod3_corona_trace, game/coronas.cpp).
  rex::cvar::SetFlagByName("cod3_corona_trace", "true");
  REXLOG_INFO("cod3: native Vulkan: the GPU emulation is off; ngl_vk draws every frame into the window (the coronas "
              "trace the world: cod3_corona_trace)");
  return true;
#else
  (void)config;
  REXLOG_WARN("cod3: native Vulkan: this build has no ngl_vk (Vulkan or glslc missing); the emulated GPU draws");
  return false;
#endif
}

// Diagnostics: which shader pairs the grass's draws use (draws kGrassDrawProbed to kGrassCensusEnd), to see whether
// the grass is drawn with more than the one vertex shader translated in grass.vert.
constexpr int kGrassCensusEnd = kGrassDrawProbed + 2000;
std::mutex g_grass_census_mutex;
std::map<std::pair<uint32_t, uint32_t>, int> g_grass_census;

void CountGrassShaders(uint8_t* base, int index) {
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  const uint32_t vs = g::ReadU32(base, device + kDeviceVertexShader), ps = g::ReadU32(base, device + kDevicePixelShader);
  const uint32_t vs_block = GrassReadable(vs) ? g::ReadU32(base, vs + 0x1C) : 0;
  const uint32_t ps_block = GrassReadable(ps) ? g::ReadU32(base, ps + 0x18) : 0;
  std::lock_guard<std::mutex> lock(g_grass_census_mutex);
  ++g_grass_census[{vs_block, ps_block}];
  if (index != kGrassCensusEnd - 1) return;
  std::string text;
  for (const auto& [pair, count] : g_grass_census) {
    char item[64];
    std::snprintf(item, sizeof item, " vs %08X ps %08X: %d;", pair.first, pair.second, count);
    text += item;
  }
  REXLOG_INFO("cod3: ngl side: grass draws {} to {} by shader block:{} pixel c255 ({} {} {} {})", kGrassDrawProbed,
              kGrassCensusEnd - 1, text, g::ReadF32(base, device + kDevicePixelConstants + 255 * 16),
              g::ReadF32(base, device + kDevicePixelConstants + 255 * 16 + 4),
              g::ReadF32(base, device + kDevicePixelConstants + 255 * 16 + 8),
              g::ReadF32(base, device + kDevicePixelConstants + 255 * 16 + 12));
}

thread_local uint32_t t_pass_object = 0;  // side91: the object whose pass is being drawn (a gun's), else 0
thread_local uint32_t t_front_object = 0;  // side116: the same, kept for all of the pass's draws (until the next pass)
thread_local uint32_t t_lit_object = 0;  // side184: every pass's object, kept for its draws (until the next pass)
// [rexglue-native] 2026-11-09 rn25c: whether this pass's light block (kModelLights, kModelLightRows) is its object's.
// 0x82142F20, the pass's parameters, refills the block (0x82142360) only while the scene being drawn has its +0x328
// set ([0x8290E9AC]+0x328, the scene's render target; dev/waw-decomp/cod3/decomp/82140000.c line 2549). rn25c: by
// frame 2900 of The Mace 360,000 lit draws came in passes of scenes whose +0x328 is 0 (A8AC7000, A8AC7550; from
// 0x82144364), with whatever block the last refill left (row xx 0.58, no point light), and each overwrote its
// object's kept light. Now only passes that refill it. (rn25b's "the soldier's rows don't match" was wrong: those GPU
// fabric draws were the view model's sleeves, c16 the eye; the soldier's kept rows, 0.198, were the same before and
// after this change.)
thread_local bool t_light_block_filled = false;

// side103: the shaders the rest of the world is drawn with. Once in play (the mesh section probe's draw count), each
// new pixel shader at any draw, up to kShadersCensused, is saved with its vertex shader (ngl-side-draw<N>-vs/-ps-*.bin;
// results/acct2-bro-tools/match_shader.py names them) and logged with its pixel constants: the lightmapped surfaces'
// scale and added light (c16, c17 in shader_F38DD169E44123DB) among them.
constexpr int kShadersCensused = 64;  // side155: 64 (24 were filled before the vehicles and the ground's blend)

// side155: the textures bound at a draw (each texture fetch constant's slot, base and format), to tell which material
// a censused pixel shader draws (SurveyMaterial lists each material's textures the same way)
std::string BoundTextures(uint8_t* base, uint32_t device) {
  std::string text;
  for (int f = 0; f < kFetchConstants; ++f) {
    const uint32_t at = device + kDeviceFetchConstants + kFetchConstantBytes * static_cast<uint32_t>(f);
    const uint32_t word0 = g::ReadU32(base, at), word1 = g::ReadU32(base, at + 4);
    if ((word0 & 3) != 2 || !word1) continue;
    char item[48];
    std::snprintf(item, sizeof item, " tf%d %08X/%u", f, word1 & ~0xFFFu, word1 & 0x3F);
    text += item;
  }
  return text.empty() ? " none" : text;
}

void CensusShader(uint8_t* base, uint32_t call_site) {
  static std::atomic<bool> done{false};
  static std::mutex census_mutex;
  static std::unordered_set<uint32_t> seen;
  static int saved = 0;
  if (done.load(std::memory_order_relaxed)) return;
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  const uint32_t vertex_shader = g::ReadU32(base, device + kDeviceVertexShader);
  const uint32_t pixel_shader = g::ReadU32(base, device + kDevicePixelShader);
  int index;
  {
    std::lock_guard lock(census_mutex);
    if (saved >= kShadersCensused) {
      done.store(true, std::memory_order_relaxed);
      return;
    }
    if (!seen.insert(pixel_shader).second) return;
    index = saved++;
  }
  REXLOG_INFO("cod3: ngl side: shader census {} (a draw from {:08X}): vertex shader {:08X}, pixel shader {:08X}; pixel "
              "constants:{}",
              index, call_site, vertex_shader, pixel_shader,
              ConstantsText(base, device + kDevicePixelConstants, nullptr));
  REXLOG_INFO("cod3: ngl side: shader census {} (frame {}): textures bound:{}", index, g_frames,
              BoundTextures(base, device));
  const std::string prefix = "ngl-side-draw" + std::to_string(index);
  SaveShader(base, vertex_shader, 0x368, 0x1C, true, prefix);
  SaveShader(base, pixel_shader, 0x28, 0x18, false, prefix);
}

#if defined(COD3_NGL_VK) && !defined(COD3_MP)
// [rexglue-native] 2026-11-09 rn20: the per-pixel lit models' shaders as the game draws them. The Mace's near soldier
// is far darker natively than in the game with the same sun and irradiance rows on both sides (rn18), and neither the
// native sun shadow, the light set's own lights nor the rows change that (rn19). So: each new pixel shader at a lit
// model's draw whose constant table names IrradianceCoeffs_0, up to kModelShadersProbed, is saved with its vertex
// shader (ngl-side-model<N>-vs/-ps) and logged with its object, its pixel and vertex constants and its textures, to
// compare with what the native mesh shader does. Debug only (cod3_debug_ngl_side).
constexpr size_t kModelShadersProbed = 6;
void ProbeModelShader(uint8_t* base, uint32_t call_site, uint32_t object) {
  static std::mutex mutex;
  static std::vector<uint32_t> probed;
  static std::unordered_set<uint32_t> other;  // shaders already found not to name the rows
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  const uint32_t pixel_shader = g::ReadU32(base, device + kDevicePixelShader);
  int index = -1;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (probed.size() >= kModelShadersProbed || other.count(pixel_shader) ||
        std::find(probed.begin(), probed.end(), pixel_shader) != probed.end()) {
      return;
    }
    if (!ShaderNames(base, pixel_shader, 0x28, "IrradianceCoeffs_0")) {
      if (other.size() < 1024) other.insert(pixel_shader);
      return;
    }
    index = static_cast<int>(probed.size());
    probed.push_back(pixel_shader);
  }
  const std::string prefix = "ngl-side-model" + std::to_string(index);
  const uint32_t vertex_shader = g::ReadU32(base, device + kDeviceVertexShader);
  REXLOG_INFO("cod3: ngl side: {} (object {:08X}, a draw from {:08X}, frame {}): vertex shader {:08X}, pixel shader "
              "{:08X}; textures bound:{}",
              prefix, object, call_site, g_frames, vertex_shader, pixel_shader, BoundTextures(base, device));
  REXLOG_INFO("cod3: ngl side: {}: pixel constants:{}", prefix,
              ConstantsText(base, device + kDevicePixelConstants, nullptr));
  REXLOG_INFO("cod3: ngl side: {}: vertex constants:{}", prefix,
              ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  SaveShader(base, vertex_shader, 0x368, 0x1C, true, prefix);
  SaveShader(base, pixel_shader, 0x28, 0x18, false, prefix);
}
#endif

// side184: the light the game hands a lit model's draw (kModelLights, kModelLightRows), kept for its native draw
void KeepModelLight(uint8_t* base, uint32_t object) {
  GameModelLight light;
  bool any = false;
  for (uint32_t r = 0; r < 9; ++r) {
    for (uint32_t c = 0; c < 3; ++c) {
      const float v = g::ReadF32(base, kModelLightRows + 16 * r + 4 * c);
      if (!std::isfinite(v) || std::fabs(v) > 1000.0f) return;
      light.rows[r][c] = v;
      any = any || v != 0.0f;
    }
  }
  if (!any) return;
  bool point = true;
  for (uint32_t i = 0; i < 3; ++i) {  // the nearest point light: +0x30 its color, +0x40 its place, +0x50 1 / radius
    light.point[i] = g::ReadF32(base, kModelLights + 0x40 + 4 * i);
    light.point[4 + i] = g::ReadF32(base, kModelLights + 0x30 + 4 * i);
  }
  light.point[3] = g::ReadF32(base, kModelLights + 0x50);
  {  // rn42w: the main light (+0x00 its color, +0x10 its way), kept for cod3_debug_ngl_side_model_sun
    bool numbers = true;
    float way_length = 0.0f;
    for (uint32_t i = 0; i < 3; ++i) {
      light.main_color[i] = g::ReadF32(base, kModelLights + 4 * i);
      light.main_way[i] = g::ReadF32(base, kModelLights + 0x10 + 4 * i);
      numbers = numbers && std::isfinite(light.main_color[i]) && std::isfinite(light.main_way[i]) &&
                light.main_color[i] >= 0.0f && light.main_color[i] <= kSunColorMost;
      way_length += light.main_way[i] * light.main_way[i];
    }
    light.main = numbers && std::isfinite(way_length) && way_length > 1e-8f;
  }
#if defined(COD3_NGL_VK) && !defined(COD3_MP)
  // [rexglue-native] 2026-11-09 rn23: rn22 read the GPU's own constants at the soldiers' draws (LogWatched): a
  // LIGHT_COLOR1 of 3.75 and irradiance rows of 0.19 with direction terms, where the native side had a point light
  // of 10 with 1 / radius 0 (dropped) and rows of 0.29-0.37. To learn how the game turns these numbers into its
  // constants, the CPU side of the same draws: up to kPairedDraws lit draws from frame 1500 on, each with the world
  // matrix's translation from the vertex constants (c0.w c1.w c2.w, the key LogWatched prints too), the main light,
  // the point light as read here, and the red of the nine rows. Debug only (cod3_debug_ngl_side).
  // rn25: from frame 2900 (the near soldier of rn18-rn24's crops at 3000), and only lights not logged before (the
  // GPU side's lines are kept to new values the same way), so the soldiers' values show beside the view model's
  {
    constexpr int kPairedDraws = 40;
    static std::atomic<int> paired{0};
    static std::mutex seen_mutex;
    static std::vector<uint64_t> seen;
    const uint32_t device = g::ReadU32(base, kDevicePointer);
    bool fresh = false;
    // rn25b: only blocks with a lit nearest point light (rn25's 40 lines were all spent on frame 2900's unlit ones)
    const bool lit_point = g::ReadF32(base, kModelLights + 0x30) + g::ReadF32(base, kModelLights + 0x34) +
                               g::ReadF32(base, kModelLights + 0x38) > 0.0f;
    if (REXCVAR_GET(cod3_debug_ngl_side) && g_frames >= 2900 && lit_point &&
        paired.load(std::memory_order_relaxed) < kPairedDraws) {
      uint64_t signature = 1469598103934665603ull;
      for (uint32_t at = 0; at < 0x180; at += 4) {  // the block and the rows, rounded to 1/1000
        const float v = g::ReadF32(base, kModelLights + at);
        const double rounded = std::isfinite(v) ? std::round(std::clamp(double{v}, -1e9, 1e9) * 1000.0) : 0.0;
        signature = (signature ^ static_cast<uint64_t>(static_cast<int64_t>(rounded))) * 1099511628211ull;
      }
      std::lock_guard<std::mutex> lock(seen_mutex);
      fresh = std::find(seen.begin(), seen.end(), signature) == seen.end();
      if (fresh) seen.push_back(signature);
    }
    if (fresh && DeviceReadable(device) && paired.fetch_add(1, std::memory_order_relaxed) < kPairedDraws) {
      const uint32_t vc = device + kDeviceVertexConstants;
      std::string rows;
      char item[40];
      for (uint32_t r = 0; r < 9; ++r) {
        std::snprintf(item, sizeof item, " %.4g", g::ReadF32(base, kModelLightRows + 16 * r));
        rows += item;
      }
      REXLOG_INFO("cod3: ngl side: rn23 cpu draw of {:08X} (frame {}): key {:.4g} {:.4g} {:.4g}; main light {:.3f} "
                  "{:.3f} {:.3f}; point at {:.1f} {:.1f} {:.1f}, 1 / radius {:.6f}, color {:.3f} {:.3f} {:.3f} (w "
                  "{:.4g}); rows (red) xx yy zz xy yz zx x y z:{}",
                  object, g_frames, g::ReadF32(base, vc + 12), g::ReadF32(base, vc + 28), g::ReadF32(base, vc + 44),
                  g::ReadF32(base, kModelLights), g::ReadF32(base, kModelLights + 4),
                  g::ReadF32(base, kModelLights + 8), light.point[0], light.point[1], light.point[2], light.point[3],
                  light.point[4], light.point[5], light.point[6], g::ReadF32(base, kModelLights + 0x3C), rows);
    }
  }
#endif
  for (float v : light.point) point = point && std::isfinite(v);
  if (!point || !(light.point[3] > 0.0f) || !(light.point[4] + light.point[5] + light.point[6] > 0.0f)) {
    for (float& v : light.point) v = 0.0f;
  }
  light.frame = g_frames;
  static std::atomic<int> shown{0};
  if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
    std::string rows;
    char item[48];
    for (uint32_t r = 0; r < 9; ++r) {
      std::snprintf(item, sizeof item, " (%.3f %.3f %.3f)", light.rows[r][0], light.rows[r][1], light.rows[r][2]);
      rows += item;
    }
    REXLOG_INFO("cod3: ngl side: object {:08X}'s lit draw (frame {}): irradiance rows xx yy zz xy yz zx x y z:{}; "
                "nearest point light {:.1f} {:.1f} {:.1f}, 1 / radius {:.5f}, color {:.2f} {:.2f} {:.2f}",
                object, light.frame, rows, light.point[0], light.point[1], light.point[2], light.point[3],
                light.point[4], light.point[5], light.point[6]);
    // rn18: and what else the game hands that draw: the main light (kModelLights +0x00 its color, +0x10 its way) and
    // the pixel shader's constants the model shaders name LIGHT_COLOR1 (c17), position_Light0 (c27) and
    // __1_COD_SHADOW_PROJCOLOR_cmn (c28), as the device holds them at the draw (a check: is there a sun in The Mace?)
    std::string constants = " -";
#if defined(COD3_NGL_VK)  // (DeviceReadable)
    const uint32_t device = g::ReadU32(base, kDevicePointer);
    if (DeviceReadable(device)) {
      constants.clear();
      for (uint32_t c : {17u, 27u, 28u}) {
        const uint32_t at = device + kDevicePixelConstants + 16 * c;
        char constant[128];
        std::snprintf(constant, sizeof constant, " c%u (%g %g %g %g)", c, g::ReadF32(base, at),
                      g::ReadF32(base, at + 4), g::ReadF32(base, at + 8), g::ReadF32(base, at + 12));
        constants += constant;
      }
    }
#endif
    REXLOG_INFO("cod3: ngl side: object {:08X}'s lit draw: main light color {:.3f} {:.3f} {:.3f}, way {:.3f} {:.3f} "
                "{:.3f}; pixel{}",
                object, g::ReadF32(base, kModelLights), g::ReadF32(base, kModelLights + 4),
                g::ReadF32(base, kModelLights + 8), g::ReadF32(base, kModelLights + 0x10),
                g::ReadF32(base, kModelLights + 0x14), g::ReadF32(base, kModelLights + 0x18), constants);
  }
  // rn40j: where the model stands and its light set, for models drawn there whose own objects are made anew
  const bool by_place = REXCVAR_GET(cod3_debug_ngl_side_light_by_place);
  if (by_place) {
    light.placed = ObjectPlace(base, object, light.at);
    light.set = ObjectLightSet(base, object);
  }
  light.object = object;
  std::lock_guard<std::mutex> lock(g_model_light_mutex);
  // rn43a: this pass's values as the game draws the object (cod3_debug_ngl_side_drawn_values), at the offsets the
  // native side reads from its block
  const uint32_t block = t_lit_values;
  const auto noted = block && REXCVAR_GET(cod3_debug_ngl_side_drawn_values) ? g_drawn_offsets.find(block)
                                                                             : g_drawn_offsets.end();
  if (noted != g_drawn_offsets.end() && noted->second.count) {
    DrawnValues drawn;
    drawn.frame = g_frames;
    drawn.block = block;
    drawn.placed = light.placed;  // rn43c
    for (int i = 0; i < 3; ++i) drawn.at[i] = light.at[i];
    for (uint32_t k = 0; k < noted->second.count; ++k) {
      const uint32_t from = block + noted->second.offsets[k];
      if (!GrassReadable(from) || !GrassReadable(from + 12)) continue;
      drawn.offsets[drawn.count] = noted->second.offsets[k];
      for (uint32_t i = 0; i < 4; ++i) drawn.values[drawn.count][i] = g::ReadF32(base, from + 4 * i);
      ++drawn.count;
    }
    const uint64_t key = DrawnValuesKey(object, block);
    if (g_drawn_values.size() >= kMaxDrawnValues && !g_drawn_values.count(key)) {
      for (auto it = g_drawn_values.begin(); it != g_drawn_values.end();) {  // those too old first, as below
        if (it->second.frame + kModelLightFrames < g_frames) {
          it = g_drawn_values.erase(it);
        } else {
          ++it;
        }
      }
      if (g_drawn_values.size() >= kMaxDrawnValues) g_drawn_values.clear();
    }
    if (drawn.count) g_drawn_values[key] = drawn;
    if (drawn.count && drawn.placed) {  // rn43c: and by where the model stood, for its block
      if (g_drawn_cells.size() >= kMaxDrawnCells) g_drawn_cells.clear();
      DrawnCell& cell = g_drawn_cells[DrawnCellKey(block, KeptLightCellOf(drawn.at[0]), KeptLightCellOf(drawn.at[1]),
                                                   KeptLightCellOf(drawn.at[2]))];
      int slot = 0;  // its own entry, else an empty one, else the oldest (as the lights' cells)
      for (int k = 0; k < kKeptLightsPerCell; ++k) {
        if (cell.objects[k] == object && cell.values[k].block == block) {
          slot = k;
          break;
        }
        if (!cell.objects[slot]) continue;
        if (!cell.objects[k] || cell.values[k].frame < cell.values[slot].frame) slot = k;
      }
      cell.objects[slot] = object;
      cell.values[slot] = drawn;
    }
  }
  if (g_model_lights.size() >= kMaxModelLights && !g_model_lights.count(object)) {
    // rn42w: drop the ones too old to be used, and all only if that isn't enough
    for (auto it = g_model_lights.begin(); it != g_model_lights.end();) {
      if (it->second.frame + kModelLightFrames < g_frames) {
        it = g_model_lights.erase(it);
      } else {
        ++it;
      }
    }
    if (g_model_lights.size() >= kMaxModelLights) g_model_lights.clear();
  }
  g_model_lights[object] = light;
  if (!by_place || !light.placed) return;
  if (g_kept_light_cells.size() >= kMaxKeptLightCells) g_kept_light_cells.clear();
  KeptLightCell& cell = g_kept_light_cells[KeptLightCellKey(KeptLightCellOf(light.at[0]), KeptLightCellOf(light.at[1]),
                                                            KeptLightCellOf(light.at[2]))];
  int slot = 0;  // its own entry, else an empty one, else the oldest
  for (int k = 0; k < kKeptLightsPerCell; ++k) {
    if (cell.objects[k] == object) {
      slot = k;
      break;
    }
    if (!cell.objects[slot]) continue;
    if (!cell.objects[k] || cell.lights[k].frame < cell.lights[slot].frame) slot = k;
  }
  cell.objects[slot] = object;
  cell.lights[slot] = light;
}

// side116c-d: the order of the front objects' passes and their draws, the first few times they come (side116c's one
// frame, the one before the third dump, had none: the front objects' places recur only on some frames)
void OnPass(uint8_t* base, uint32_t object, uint32_t state) {
  t_lit_object = object;
  // rn43a: the value block this pass fills for its object (the shader state's +0x1C, as ReadPassBinding reads it)
  t_lit_values = 0;
  if (state && GrassReadable(state) && GrassReadable(state + 0x1C)) {
    const uint32_t values = g::ReadU32(base, state + 0x1C);
    if (GrassReadable(values)) t_lit_values = values;
  }
  {  // rn25c: the game's own test before it refills the light block (0x82142F20)
    const uint32_t scene = g::ReadU32(base, kCurrentScene);
    t_light_block_filled = Pointer(scene) && Readable(scene + 0x328) && g::ReadU32(base, scene + 0x328) != 0;
  }
  // [rexglue-native] 2026-11-10 rn40h: a watched helmet object's passes; rn40i: and the passes of objects near one
  if (REXCVAR_GET(cod3_debug_ngl_side) && g_watched_light_count.load(std::memory_order_relaxed) != 0) {
    float at[3] = {};
    const bool placed = Pointer(object) && Readable(object + 0x38);
    for (int i = 0; placed && i < 3; ++i) at[i] = g::ReadF32(base, object + 0x30 + 4 * i);
    uint32_t near_one = 0;
    const int watched = WatchedLightObject(object, placed ? at : nullptr, &near_one);
    static std::atomic<int> shown[2];  // (zero as statics)
    if (watched && shown[watched - 1].fetch_add(1, std::memory_order_relaxed) < 24) {
      char near_text[24] = "";
      if (watched == 2) std::snprintf(near_text, sizeof near_text, " (near %08X)", near_one);
      REXLOG_INFO("cod3: ngl side: rn40i pass of {} {:08X} (frame {}) at {:.1f} {:.1f} {:.1f}{}: the light block "
                  "refilled {}",
                  watched == 1 ? "watched object" : "an object near one,", object, g_frames, at[0], at[1], at[2],
                  near_text, t_light_block_filled ? 1 : 0);
    }
  }
  t_pass_object = 0;
  t_front_object = 0;
  if (!Watching()) return;
  std::lock_guard<std::mutex> lock(g_front_mutex);
  const bool front = g_front_objects.count(object) != 0;
  if (front) t_front_object = object;
  if (front && REXCVAR_GET(cod3_debug_ngl_side)) t_pass_object = object;
  static thread_local bool last_front = false;  // the pass before was a front object's (to see where they end)
  static std::atomic<int> shown{0};
  if ((front || last_front) && REXCVAR_GET(cod3_debug_ngl_side) &&
      shown.fetch_add(1, std::memory_order_relaxed) < 24) {
    REXLOG_INFO("cod3: ngl side: light order: pass of {:08X}{} (frame {})", object, front ? " (a front object)" : "",
                g_frames);
  }
  last_front = front;
}

bool WaitForRenderJobFirst() {
  // side224: only where it matters (native mode); the game waits for the same job right after anyway
  // rn42n: and for the compare pictures (cod3_debug_ngl_side), whose effects split between two lists the same way
  // (rn42m: 114 then 57, 237 then 19 flushes on frames in a row)
  return (g_native.load(std::memory_order_relaxed) || REXCVAR_GET(cod3_debug_ngl_side)) &&
         REXCVAR_GET(cod3_native_effects_whole_list);
}

bool SkipGameDraw() {
  const bool skip = t_skip_game_draw;
  t_skip_game_draw = false;
  if (skip) return true;
  // side222: in native mode the game's draws are drawn by no one (cod3_native_skip_game_draws)
  return g_native.load(std::memory_order_relaxed) && REXCVAR_GET(cod3_native_skip_game_draws) &&
         !REXCVAR_GET(cod3_debug_host_draws);
}

void OnDraw(uint8_t* base, uint32_t call_site) {
  if (REXCVAR_GET(cod3_debug_ngl_side)) CountDrawSite(call_site);  // rn13
  {  // rn13: cod3_debug_skip_game_call_site
    const int32_t skip_site = REXCVAR_GET(cod3_debug_skip_game_call_site);
    if (skip_site != 0 && static_cast<uint32_t>(skip_site) == call_site) t_skip_game_draw = true;
  }
  if (REXCVAR_GET(cod3_debug_ngl_side) &&
      g_mesh_section_draws.load(std::memory_order_relaxed) >= kMeshSectionProbed) {
    CensusShader(base, call_site);
  }
  if (t_pass_object && REXCVAR_GET(cod3_debug_ngl_side)) {  // side91: a draw of the gun: what the GPU is handed
    static std::atomic<int> probed{0};
    const uint32_t device = g::ReadU32(base, kDevicePointer);
    if (DeviceReadable(device) && probed.fetch_add(1, std::memory_order_relaxed) < 3) {
      REXLOG_INFO("cod3: ngl side: gun draw (object {:08X}, from {:08X}): vertex constants:{}", t_pass_object,
                  call_site, ConstantsText(base, device + kDeviceVertexConstants, nullptr));
      REXLOG_INFO("cod3: ngl side: gun draw: object +40: {} / {}", Words(base, t_pass_object + 0x40, 8),
                  Words(base, t_pass_object + 0x60, 8));
    }
    t_pass_object = 0;
  }
#if !defined(COD3_MP)
  // side116: the light a front object's lit draws are handed, for its native draw. side116b looked for them among the
  // mesh sections' draws and found none in a front pass; the gun's draws all return to the models' draw
  // (kModelDrawReturn), where the main light comes two ways (the sun's color 1.097 0.994 0.780 and 0.670 0.564 0.442,
  // both with the sun's far place as the way). side116c: every draw in a front pass is logged with its shaders, to tell
  // the lit draw from the rest; the last model draw's light is kept. side116d: each new pixel shader a front draw uses
  // (up to four) is saved as ngl-side-front<N> with its vertex shader (census_match.py RUN front SHOTS DUMP names
  // them), with the vertex constants it is handed (c18 the sun's way, c19 its color / 1.75 at the gun's first draw).
  const bool front_draw = t_front_object && Watching();
  if (front_draw && REXCVAR_GET(cod3_debug_ngl_side)) {
    static std::atomic<int> front_shown{0};
    static std::mutex saved_mutex;
    static std::vector<uint32_t> saved_pixel_shaders;
    const uint32_t device = g::ReadU32(base, kDevicePointer);
    const bool readable = DeviceReadable(device);
    const uint32_t vertex_shader = readable ? g::ReadU32(base, device + kDeviceVertexShader) : 0;
    const uint32_t pixel_shader = readable ? g::ReadU32(base, device + kDevicePixelShader) : 0;
    int save_as = -1;
    if (readable) {
      std::lock_guard<std::mutex> lock(saved_mutex);
      if (saved_pixel_shaders.size() < 4 &&
          std::find(saved_pixel_shaders.begin(), saved_pixel_shaders.end(), pixel_shader) ==
              saved_pixel_shaders.end()) {
        save_as = static_cast<int>(saved_pixel_shaders.size());
        saved_pixel_shaders.push_back(pixel_shader);
      }
    }
    if (save_as >= 0) {
      const std::string prefix = "ngl-side-front" + std::to_string(save_as);
      REXLOG_INFO("cod3: ngl side: {} (front object {:08X}, a draw from {:08X}): vertex constants:{}", prefix,
                  t_front_object, call_site, ConstantsText(base, device + kDeviceVertexConstants, nullptr));
      SaveShader(base, vertex_shader, 0x368, 0x1C, true, prefix);
      SaveShader(base, pixel_shader, 0x28, 0x18, false, prefix);
    }
    if (readable && front_shown.fetch_add(1, std::memory_order_relaxed) < 60) {
      REXLOG_INFO("cod3: ngl side: light order: draw from {:08X} (front object {:08X}), shaders {:08X} / {:08X}, main "
                  "light color {:.3f} {:.3f} {:.3f} {:.3f} way {:.1f} {:.1f} {:.1f}, red in rows xx {:.3f} yy {:.3f} zz "
                  "{:.3f}",
                  call_site, t_front_object, g::ReadU32(base, device + kDeviceVertexShader),
                  g::ReadU32(base, device + kDevicePixelShader), g::ReadF32(base, kModelLights),
                  g::ReadF32(base, kModelLights + 4), g::ReadF32(base, kModelLights + 8),
                  g::ReadF32(base, kModelLights + 12), g::ReadF32(base, kModelLights + 0x10),
                  g::ReadF32(base, kModelLights + 0x14), g::ReadF32(base, kModelLights + 0x18),
                  g::ReadF32(base, kModelLightRows), g::ReadF32(base, kModelLightRows + 16),
                  g::ReadF32(base, kModelLightRows + 32));
    }
  }
  if (front_draw && call_site == kModelDrawReturn) {
    FrontLight light;
    bool numbers = true;
    for (uint32_t i = 0; i < 3; ++i) {
      light.color[i] = g::ReadF32(base, kModelLights + 4 * i);
      light.way[i] = g::ReadF32(base, kModelLights + 0x10 + 4 * i);
      light.ambient[i] = (g::ReadF32(base, kModelLightRows + 4 * i) + g::ReadF32(base, kModelLightRows + 16 + 4 * i) +
                          g::ReadF32(base, kModelLightRows + 32 + 4 * i)) / 3.0f;
      numbers = numbers && std::isfinite(light.color[i]) && std::isfinite(light.way[i]) &&
                std::isfinite(light.ambient[i]);
    }
    if (numbers) {
      static std::atomic<int> kept_shown{0};
      if (kept_shown.fetch_add(1, std::memory_order_relaxed) < 4) {
        REXLOG_INFO("cod3: ngl side: front object {:08X}'s draw light: color {:.3f} {:.3f} {:.3f}, way {:.3f} {:.3f} "
                    "{:.3f}, ambient {:.3f} {:.3f} {:.3f}", t_front_object, light.color[0], light.color[1],
                    light.color[2], light.way[0], light.way[1], light.way[2], light.ambient[0], light.ambient[1],
                    light.ambient[2]);
        // side134: the whole light the game hands it: the nine rows of its quadratic form in the normal (xx, yy, zz,
        // the cross terms, x, y, z; the arms' pixel shader CDBAC6026B525050 reads them as c18-c26, the sun as c27),
        // and the pixel constants the draw was given, to match the native ambient to the game's
        std::string rows;
        char item[64];
        for (uint32_t r = 0; r < 9; ++r) {
          std::snprintf(item, sizeof item, " [%.3f %.3f %.3f %.3f]", g::ReadF32(base, kModelLightRows + 16 * r),
                        g::ReadF32(base, kModelLightRows + 16 * r + 4), g::ReadF32(base, kModelLightRows + 16 * r + 8),
                        g::ReadF32(base, kModelLightRows + 16 * r + 12));
          rows += item;
        }
        const uint32_t device = g::ReadU32(base, kDevicePointer);
        REXLOG_INFO("cod3: ngl side: front object {:08X}'s light rows:{}; main light w {:.3f}; pixel constants:{}",
                    t_front_object, rows, g::ReadF32(base, kModelLights + 12),
                    DeviceReadable(device) ? ConstantsText(base, device + kDevicePixelConstants, nullptr)
                                           : std::string(" -"));
      }
      std::lock_guard<std::mutex> lock(g_front_mutex);
      if (g_front_lights.size() < 64 || g_front_lights.count(t_front_object)) g_front_lights[t_front_object] = light;
    }
  }
#endif
  // side184: every lit model's draws (the models' draw and the mesh sections'), for
  // cod3_debug_ngl_side_model_irradiance (side194: cod3mp's too)
  const bool lit_draw =
      call_site == kModelDrawReturn || (call_site >= kMeshSectionStart && call_site < kMeshSectionEnd);
  // [rexglue-native] 2026-11-10 rn40h: a watched helmet object's draws: from where, taken as lit or not, and the light
  // block's rows then (red xx yy zz)
  if (t_lit_object && REXCVAR_GET(cod3_debug_ngl_side) &&
      WatchedLightObject(t_lit_object, nullptr, nullptr) == 1) {
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 24) {
      REXLOG_INFO("cod3: ngl side: rn40h draw of watched object {:08X} from {:08X} (frame {}): a lit draw {}, the "
                  "light block refilled {}, its rows (red) xx {:.4f} yy {:.4f} zz {:.4f}",
                  t_lit_object, call_site, g_frames, lit_draw ? 1 : 0, t_light_block_filled ? 1 : 0,
                  g::ReadF32(base, kModelLightRows), g::ReadF32(base, kModelLightRows + 16),
                  g::ReadF32(base, kModelLightRows + 32));
    }
  }
  if (t_lit_object && lit_draw && REXCVAR_GET(cod3_debug_ngl_side_model_irradiance) != 0 && Watching()) {
    if (t_light_block_filled) {
      KeepModelLight(base, t_lit_object);
    } else if (REXCVAR_GET(cod3_debug_ngl_side)) {  // rn25c: what was overwriting the kept light, and from where
      const uint64_t stale = g_light_block_stale_draws.fetch_add(1, std::memory_order_relaxed);
      if (stale < 6) {
        const uint32_t scene = g::ReadU32(base, kCurrentScene);
        REXLOG_INFO("cod3: ngl side: rn25c lit draw of {:08X} from {:08X} (frame {}) in a pass that didn't refill the "
                    "light block: scene {:08X}, its +0x328 {:08X}; the block's row xx red {:.4f}, point light color "
                    "{:.3f} {:.3f} {:.3f}",
                    t_lit_object, call_site, g_frames, scene,
                    Pointer(scene) && Readable(scene + 0x328) ? g::ReadU32(base, scene + 0x328) : 0u,
                    g::ReadF32(base, kModelLightRows), g::ReadF32(base, kModelLights + 0x30),
                    g::ReadF32(base, kModelLights + 0x34), g::ReadF32(base, kModelLights + 0x38));
      }
    }
  }
#if defined(COD3_NGL_VK) && !defined(COD3_MP)
  // rn20: the per-pixel lit models' shaders and constants as the game draws them (debug only)
  if (t_lit_object && lit_draw && REXCVAR_GET(cod3_debug_ngl_side)) ProbeModelShader(base, call_site, t_lit_object);
#endif
#if !defined(COD3_MP)
  if (t_lit_object && REXCVAR_GET(cod3_debug_ngl_side)) {  // side188: the puddles' shaders, as the game draws them
    bool water = false;
    {
      std::lock_guard<std::mutex> lock(g_water_mutex);
      water = g_water_objects.count(t_lit_object) != 0;
    }
    static std::mutex water_mutex;
    static std::vector<uint32_t> water_shaders;
    const uint32_t device = water ? g::ReadU32(base, kDevicePointer) : 0;
    if (water && DeviceReadable(device)) {
      const uint32_t pixel_shader = g::ReadU32(base, device + kDevicePixelShader);
      int save_as = -1;
      {
        std::lock_guard<std::mutex> lock(water_mutex);
        if (water_shaders.size() < 3 &&
            std::find(water_shaders.begin(), water_shaders.end(), pixel_shader) == water_shaders.end()) {
          save_as = static_cast<int>(water_shaders.size());
          water_shaders.push_back(pixel_shader);
        }
      }
      if (save_as >= 0) {
        const std::string prefix = "ngl-side-water" + std::to_string(save_as);
        REXLOG_INFO("cod3: ngl side: {} (object {:08X}, a draw from {:08X}): blend {:08X}; pixel constants:{}",
                    prefix, t_lit_object, call_site, g::ReadU32(base, device + kDeviceBlendControl0),
                    ConstantsText(base, device + kDevicePixelConstants, nullptr));
        REXLOG_INFO("cod3: ngl side: {}: vertex constants:{}", prefix,
                    ConstantsText(base, device + kDeviceVertexConstants, nullptr));
        SaveShader(base, g::ReadU32(base, device + kDeviceVertexShader), 0x368, 0x1C, true, prefix);
        SaveShader(base, pixel_shader, 0x28, 0x18, false, prefix);
      }
    }
  }
#endif
  if (call_site >= kMeshSectionStart && call_site < kMeshSectionEnd && Watching()) {
    if (REXCVAR_GET(cod3_debug_ngl_side)) {
      const int index = g_mesh_section_draws.fetch_add(1, std::memory_order_relaxed);
      if (index >= kMeshSectionProbed && index < kMeshSectionProbed + kMeshSectionsLogged) {
        ProbeMeshSection(base, call_site, index);
      }
    }
    KeepSkyDraw(base);
  }
#if defined(COD3_NGL_VK)
  if (call_site == kEffectDrawReturn && Watching()) KeepEffectDraw(base);  // side104, side110
  // rn12: cod3_debug_skip_game_effect, the effect flushes with this texture left out of the game's own picture
  if (call_site == kEffectDrawReturn) {
    const int32_t skip = REXCVAR_GET(cod3_debug_skip_game_effect);
    const uint32_t device = skip != 0 ? g::ReadU32(base, kDevicePointer) : 0;
    if (skip != 0 && DeviceReadable(device) &&
        static_cast<uint32_t>(skip) == (g::ReadU32(base, device + kDeviceFetchConstants + 4) & 0x1FFFF000)) {
      t_skip_game_draw = true;
    }
  }
#endif
#if defined(COD3_NGL_VK) && !defined(COD3_MP)
  if (call_site == kPlainDrawReturn && REXCVAR_GET(cod3_debug_ngl_side)) ProbePlainDraw(base);  // rn15
  if (call_site == kPlainDrawReturn && Watching()) KeepRainDraw(base);                          // rn16
#endif
#if !defined(COD3_MP)
  const bool section_draw = call_site >= kMeshSectionStart && call_site < kMeshSectionEnd;
  if ((call_site == kModelDrawReturn || section_draw) && REXCVAR_GET(cod3_debug_ngl_side) &&
      g_frames >= 3 * DumpEvery() - 1 && g_frames % DumpEvery() == DumpEvery() - 1) {
    // side107: the lights the game hands a model, on the frames before a dump; side109: the mesh sections' draws too
    // (the per-pixel lit models and the gun, shader FC767C09)
    static std::atomic<int> shown{0}, sections_shown{0};
    if ((section_draw ? sections_shown : shown).fetch_add(1, std::memory_order_relaxed) < (section_draw ? 24 : 36)) {
      auto vec = [&](uint32_t at) {
        char text[80];
        std::snprintf(text, sizeof text, "(%.3f %.3f %.3f %.3f)", g::ReadF32(base, at), g::ReadF32(base, at + 4),
                      g::ReadF32(base, at + 8), g::ReadF32(base, at + 12));
        return std::string(text);
      };
      std::string rows;
      for (uint32_t r = 0; r < 9; ++r) rows += " " + vec(kModelLightRows + 16 * r);
      REXLOG_INFO("cod3: ngl side: {} draw from {:08X} (frame {}): main light color {} way {}; point lights {} at {} / "
                  "{} at {}; rows:{}", section_draw ? "section" : "model", call_site, g_frames, vec(kModelLights),
                  vec(kModelLights + 0x10), vec(kModelLights + 0x30), vec(kModelLights + 0x40),
                  vec(kModelLights + 0x60), vec(kModelLights + 0x70), rows);
    }
  }
#endif
  if (call_site == kGrassDrawReturn && Watching()) {
    const uint32_t device = g::ReadU32(base, kDevicePointer);
    if (DeviceReadable(device)) {  // where this draw fetched the blades from (vertex fetch constant 95)
      const uint32_t at = device + kDeviceFetchConstants + kFetchConstantBytes * 31 + 16;
      g_grass_fetch.store(uint64_t{g::ReadU32(base, at)} << 32 | g::ReadU32(base, at + 4), std::memory_order_relaxed);
    }
  }
  if (call_site == kGrassDrawReturn && REXCVAR_GET(cod3_debug_ngl_side)) {
    const int index = g_grass_draws.fetch_add(1, std::memory_order_relaxed);
    if (index >= kGrassDrawProbed && index < kGrassDrawProbed + kGrassDrawsLogged) ProbeGrass(base, index);
    if (index >= kGrassDrawProbed && index < kGrassCensusEnd) CountGrassShaders(base, index);
  }
  if (g_draws_to_read.load(std::memory_order_relaxed) <= 0) return;
  if (g_draws_to_read.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  uint64_t key = call_site;
  const std::string pixel = ConstantsText(base, device + kDevicePixelConstants, &key);
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  if (g_draws_logged >= kDrawsLogged || !g_draw_keys.insert(key).second) return;
  const int logged = g_draws_logged++;
  REXLOG_INFO("cod3: ngl side: draw from {:08X} (device {:08X}): RB_FOG_COLOR {:08X}; pixel constants:{}", call_site,
              device, g::ReadU32(base, device + kDeviceFogColor), pixel);
  if (logged < kVertexDrawsLogged) {
    REXLOG_INFO("cod3: ngl side:   its vertex constants:{}",
                ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  }
}

}  // namespace cod3::ngl_side

#if !defined(COD3_MP)
// side123: the movie player's WMV GetOutput (see MovieFrames): each frame it gives out kept for the native side.
REX_EXTERN(__imp__sub_8220D4E8);
REX_HOOK_RAW(sub_8220D4E8) {
  const uint32_t buffer = ctx.r4.u32, size_out = ctx.r6.u32;
  const uint32_t state = ctx.r3.u32 ? cod3::guest::ReadU32(base, ctx.r3.u32 + 0x20) : 0;
  __imp__sub_8220D4E8(ctx, base);
  if (!cod3::ngl_side::Live() && !REXCVAR_GET(cod3_debug_ngl_side)) return;
  if (!buffer || !size_out || !state || !cod3::guest::ReadU32(base, size_out)) return;  // no frame came out
  const uint32_t width = cod3::guest::ReadU32(base, state + 0x20), height = cod3::guest::ReadU32(base, state + 0x24);
  if (width < 16 || height < 16 || width > 4096 || height > 4096 || (width & 1) || (height & 1)) return;
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_INFO("cod3: ngl side: a movie: {}x{} frames from the player's decoder", width, height);
  }
  cod3::ngl_side::KeepMovieFrame(base + buffer, width, height);
}
#endif

#if defined(COD3_NGL_VK)
// side127: post effect 10, the menus' blur (campaign 0x8214A908, cod3mp 0x821627B0; f1 its amount), as the game
// draws it: the native side blurs its own picture by the same amount (MenuBlurThisFrame).
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_821627B0);
REX_HOOK_RAW(sub_821627B0) {
  cod3::ngl_side::KeepMenuBlur(static_cast<float>(ctx.f1.f64));
  __imp__sub_821627B0(ctx, base);
}
#else
REX_EXTERN(__imp__sub_8214A908);
REX_HOOK_RAW(sub_8214A908) {
  cod3::ngl_side::KeepMenuBlur(static_cast<float>(ctx.f1.f64));
  __imp__sub_8214A908(ctx, base);
}
#endif
// side170: the apsColorUVANode's Render (campaign 0x8210FD10, r3 the node; see cod3_debug_skip_color_uv_nodes). Its
// cod3mp twin (vtable 0x82015080) is not hooked yet.
#if !defined(COD3_MP)
REX_EXTERN(__imp__sub_8210FD10);
REX_HOOK_RAW(sub_8210FD10) {
  if (cod3::ngl_side::KeepColorUvNode(base, ctx.r3.u32)) __imp__sub_8210FD10(ctx, base);
}
// side179: the Renders of the node kinds 0x82001D2C and 0x82003454 (see cod3_debug_skip_game_draws)
REX_EXTERN(__imp__sub_820D4F70);
REX_HOOK_RAW(sub_820D4F70) {
  if (cod3::ngl_side::KeepGameDraw(1)) __imp__sub_820D4F70(ctx, base);
}
REX_EXTERN(__imp__sub_820E0AB8);
REX_HOOK_RAW(sub_820E0AB8) {
  if (cod3::ngl_side::KeepGameDraw(2)) __imp__sub_820E0AB8(ctx, base);
}
#endif
// side130: the post effects' dispatcher (campaign 0x8214B0B0, cod3mp 0x82162F58; r3 the type, r4 its parameters)
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_82162F58);
REX_HOOK_RAW(sub_82162F58) {
  cod3::ngl_side::KeepPostEffect(base, ctx.r3.u32, ctx.r4.u32);
  if (cod3::ngl_side::SkipGamePostEffect(ctx.r3.u32)) return;  // side200: a check
  __imp__sub_82162F58(ctx, base);
}
#else
REX_EXTERN(__imp__sub_8214B0B0);
REX_HOOK_RAW(sub_8214B0B0) {
  cod3::ngl_side::KeepPostEffect(base, ctx.r3.u32, ctx.r4.u32);
  if (cod3::ngl_side::SkipGamePostEffect(ctx.r3.u32)) return;  // side200: a check
  __imp__sub_8214B0B0(ctx, base);
}
#endif
#endif
