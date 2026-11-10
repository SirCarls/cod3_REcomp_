// cod3 - the native renderer beside the emulated GPU, live in the window: an A/B compare with a wipe.
// [rexglue-native] 2026-11-09 (owner 2026-11-09 19:17: the live switch and wipe slider, approved)
//
// With cod3_native_compare on, game/ngl_side.cpp draws the native picture (ngl_vk) every frame besides the emulated
// GPU's, as cod3_ngl_side_live does, and hands each one here (Hand). A drawer of the window's presenter puts it over
// the emulated GPU's picture, right of a vertical line: the emulated picture is left of the line, the native one right
// of it. The native picture goes through the game's gamma table first, as it does when native mode shows it
// (ngl_side.cpp's Show), so both sides get the same colors. It covers the same rectangle of the window as the game's
// picture (game/ultrawide.h's PictureInView), so the two line up pixel for pixel when both are drawn at one size.
// The drawer is below ImGui's (the HUD overlay, F2), so those stay on top.
//
// Keys (while the console and the settings window don't have the input):
//   Ctrl+Home          the compare on or off (cod3_native_compare)
//   while it is on:
//   Ctrl+Left / Right  the line 5% left or right; with Shift 0.5% (cod3_native_compare_split)
//   Ctrl+Up            A/B: all emulated, then all native, then all emulated...
//   Ctrl+Down          the line back in the middle
// The same two settings from the console ("cod3_native_compare_split 0.25") or the command line.
//
// Not in native mode (cod3_native_vulkan): the emulated GPU draws nothing there, so there is nothing to compare with.
// While the compare is off nothing here runs but one setting check a frame on the game's thread and one per paint on
// the window's. Both programs can use it; the campaign attaches it (cod3_app.cpp).

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

namespace cod3::compare {

// The game's gamma table, per channel (red, green, blue): what each 8-bit value is shown as.
using GammaTable = std::array<std::array<uint8_t, 256>, 3>;

// UI thread, once the graphics are up (OnPostSetup): the drawer on the presenter (z_order below ImGui's 64) and the
// keys on the window (input_z_order). overlay_open says when the console or the settings window has the input (then
// the keys are left to them). Does nothing without a presenter or a provider.
void Attach(rex::ui::Window* window, rex::ui::Presenter* presenter, rex::ui::GraphicsProvider* provider,
            size_t input_z_order, std::function<bool()> overlay_open);
// UI thread, before the presenter goes (OnShutdown).
void Detach(rex::ui::Window* window);

// Whether the compare is on (cod3_native_compare) and attached. Any thread.
bool Wanted();

// The thread that draws the native frames (one frame at a time): the native picture, width x height pixels of
// bytes r, g, b, a, and the gamma table it is shown through. Copied (through the table) before this returns.
void Hand(const uint8_t* rgba, uint32_t width, uint32_t height, const GammaTable& table);

}  // namespace cod3::compare
