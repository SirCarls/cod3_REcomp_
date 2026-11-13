// cod2 - the native renderer beside the emulated GPU, live in the window: an A/B compare with a wipe.
// [waw-decomp] 2026-11-11, after Call of Duty 3's (recomp/src/render/native_compare.h, rexglue-native, 2026-11-09;
// owner 2026-11-11 07:47: "The other chat has a comparison tool, it might help. It's like a slider.")
//
// With cod2_native_compare on, game/native_side.cpp draws the native picture (ngl_vk) every frame beside the emulated
// GPU's and hands each one here (Hand). A drawer of the window's presenter puts it over the emulated GPU's picture,
// right of a vertical line: the emulated picture is left of the line, the native one right of it. The native picture
// goes through the display's gamma table first, as the saved native pictures do (native_side.cpp's ShownColors), so
// both sides get the same colors. It covers the middle of the window at the native picture's own shape, as the
// presenter shows the game's picture (letterboxed), so the two line up when both are drawn at one size. The drawer is
// below ImGui's (F2 and the console), so those stay on top.
//
// Keys (while the F2 window doesn't have the input):
//   Ctrl+Home          the compare on or off (cod2_native_compare)
//   while it is on:
//   Ctrl+Left / Right  the line 5% left or right; with Shift 0.5% (cod2_native_compare_split)
//   Ctrl+Up            A/B: all emulated, then all native, then all emulated...
//   Ctrl+Down          the line back in the middle
// The same two settings from the console ("cod2_native_compare_split 0.25") or the command line.
//
// Both programs (cod2 and cod2mp) have it. Drawing the native picture every frame costs frame rate while it is on;
// while it is off nothing here runs but one setting check a frame on the game's thread and one per paint on the
// window's. Without ngl_vk in the build (COD2_NGL_VK) there is no native picture, so the line shows the game alone.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace rex::ui {
class GraphicsProvider;
class Presenter;
class Window;
}  // namespace rex::ui

namespace cod2::compare {

// The display's gamma table, per channel (red, green, blue): what each 8-bit value is shown as.
using GammaTable = std::array<std::array<uint8_t, 256>, 3>;

// UI thread, once the graphics are up (Cod2App::OnPostSetup): the drawer on the presenter (below ImGui's) and the keys
// on the window (input_z_order). overlay_open says when the F2 window has the input (then the keys are left to it).
// Does nothing without a presenter or a provider.
void Attach(rex::ui::Window* window, rex::ui::Presenter* presenter, rex::ui::GraphicsProvider* provider,
            size_t input_z_order, std::function<bool()> overlay_open);
// UI thread, before the presenter goes (Cod2App::OnShutdown).
void Detach(rex::ui::Window* window);

// Whether the compare is on (cod2_native_compare) and attached. Any thread.
bool Wanted();

// The thread that draws the native frames (one frame at a time): the native picture, width x height pixels of bytes
// r, g, b, a, and the gamma table it is shown through. Copied (through the table) before this returns.
void Hand(const uint8_t* rgba, uint32_t width, uint32_t height, const GammaTable& table);

}  // namespace cod2::compare
