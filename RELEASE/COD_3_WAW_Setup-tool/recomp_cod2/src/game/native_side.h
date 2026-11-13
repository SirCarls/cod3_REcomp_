// cod2 - the native renderer's step 3: the frame's 2D drawn natively, beside the GPU emulation. [waw-decomp]
//
// A check only, off by default (cod2_debug_native_side): the game keeps drawing through the GPU emulation. See
// native_side.cpp and dev NATIVE-RENDERER.md section 8 (step 3). The census's hooks (native_census.cpp) call these, so
// each guest function keeps one hook.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::ui {
struct RawImage;
}

namespace cod2::native {

// True while cod2_debug_native_side is on.
bool SideOn();

// At the draw 0x82368310 (before the game's own code runs): keeps the draw when it is 2D and the frame is being kept;
// counts the indexed ones (the world and the models) by kind. pass: the pass "RB_SetupPass" 0x82368C40 last set up.
void SideDraw(const uint8_t* base, uint32_t args, uint32_t pass);

// At RB_SwapBuffers 0x82372D78 (after the game's own code runs): draws a kept frame natively and saves it.
void SideFrameEnd(const uint8_t* base);

// Where the pictures go and how the game's own picture is read (the SDK presenter's CaptureGuestOutput); from
// Cod2App::OnPostSetup. Starts the helper thread that saves the game's pictures, when the check is on.
using SideCaptureFn = std::function<bool(rex::ui::RawImage&)>;
void SideStart(const std::filesystem::path& folder, SideCaptureFn capture);

// Stops the helper thread (from Cod2App::OnShutdown, before the GPU emulation goes).
void SideStop();

}  // namespace cod2::native
