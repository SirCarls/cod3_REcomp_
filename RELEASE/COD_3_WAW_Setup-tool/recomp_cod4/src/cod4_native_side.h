// cod4 - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation: the native renderer's side check.
// [waw-decomp-cod4] 2026-11-11
//
// With cod4_debug_native_side on (off by default), the frames 600, 1200, ... 4800 are drawn a second time, natively,
// by ngl_vk (the project's ngl_vk/ folder, the Vulkan library Call of Duty 3's, Big Red One's and Call of Duty 2's
// native renderers draw with), from what the game's own back end submits for each draw, and saved beside the game's
// own picture of the same moment (userdata/logs/screenshots/ngl-side-<frame>-vk.ppm and -game.png), so
// tools/ngl_side_compare.py can score the difference. The game keeps drawing through the GPU emulation; nothing it
// does changes. The design is Call of Duty 2's (../recomp_cod2/src/game/native_side.cpp); what is read where is in
// cod4_native_side.cpp and in dev/agents/waw-decomp/COD4-NATIVE.md. The campaign program only so far; a build without
// ngl_vk (COD4_NGL_VK not defined: no Vulkan headers or glslc) only counts.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::ui {
struct RawImage;
}

namespace cod4::native {

// Reads the game's latest picture; false when there is none yet (as cod4::shots::CaptureFn).
using SideCaptureFn = std::function<bool(rex::ui::RawImage&)>;

// Whether cod4_debug_native_side is on. [waw-decomp-cod4] 2026-11-11 round 6: or cod4_native_vulkan (every frame is
// then drawn natively and shown in the window: cod4_native_window.h).
bool SideOn();

// At the Direct3D layer's indexed draw (0x8209BD80; from cod4_capture_draw, cod4_census.cpp): `context` and
// `surface_type` are the draw-list loop's for the run being drawn (kept by cod4_census_tess on this thread), `device`
// the Direct3D device the draw goes to (r3), `caller` where the draw returns to (the link register), then the draw's
// base vertex, first index and index count (r5 to r7).
void SideDraw(const uint8_t* base, uint32_t context, uint32_t surface_type, uint32_t device, uint32_t caller,
              uint32_t base_vertex, uint32_t first_index, uint32_t index_count);

// At the start of the back end's frame (0x82128488; cod4_native_frame in cod4_census.cpp): the frame before it has
// ended.
void SideFrame(const uint8_t* base);

// Once the GPU emulation is up: starts the helper thread that saves the pictures, when the check is on.
void SideStart(const std::filesystem::path& folder, SideCaptureFn capture);

// At shutdown, before the presenter goes.
void SideStop();

}  // namespace cod4::native
