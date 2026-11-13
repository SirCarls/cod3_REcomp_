// waw - World at War's native renderer: a frame's draws drawn natively by ngl_vk, beside the game's own picture.
// [waw-decomp-waw] 2026-11-11
//
// A check only, off by default (waw_debug_native_side): the game keeps drawing through the GPU emulation. See
// waw_native_side.cpp and dev/agents/waw-decomp/WAW-NATIVE.md. The hooks (waw_native.cpp) call these on the thread
// that draws.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::ui {
struct RawImage;
}

namespace waw::native {

// True while waw_debug_native_side is on, or the native picture is the game's (waw_native_vulkan,
// waw_native_window.h; [waw-decomp-waw] 2026-11-11 round 5: then every frame is drawn natively).
bool SideOn();

// One indexed draw (R_DrawIndexedPrimitive): the run's source state (0 outside a draw-list run), the command buffer
// state, the draw's arguments (+4 the triangle count, +8 the first index), the run's surface type (16: none), and
// from the caller of R_DrawIndexedPrimitive its return address and its r11 (where the model functions have just
// written a model's placement for the GPU).
void SideIndexedDraw(const uint8_t* base, uint32_t source, uint32_t state, uint32_t arguments,
                     uint32_t surface_type, uint32_t caller, uint32_t caller_r11);

// One draw from the tess buffers (0x824322B8): the arguments are +0 the vertex count, +4 the triangle count.
void SideTessDraw(const uint8_t* base, uint32_t source, uint32_t state, uint32_t arguments, uint32_t surface_type);

// The frame's end (Direct3D's swap).
void SideFrameEnd(const uint8_t* base);

// Where the pictures go and how the game's own picture is read (the SDK presenter's CaptureGuestOutput); from
// WawApp::OnPostSetup. SideStop at shutdown, before the presenter goes.
using SideCaptureFn = std::function<bool(rex::ui::RawImage&)>;
void SideStart(const std::filesystem::path& folder, SideCaptureFn capture);
void SideStop();

}  // namespace waw::native
