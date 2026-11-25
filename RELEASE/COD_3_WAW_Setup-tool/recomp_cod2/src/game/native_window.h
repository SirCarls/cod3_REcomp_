// cod2 - the native renderer as the game's own picture, with the GPU emulation off (cod2_native_vulkan). [waw-decomp]
//
// Off by default, and in development. When it is on, the GPU plugin's host backend is loaded in place of the GPU
// emulation: it reads and answers the game's GPU commands (registers, fences, interrupts, swaps) as the emulation
// does, but draws nothing. Each frame is drawn by the native side (native_side.cpp, with ngl_vk) from the game's own
// draw lists and handed to the window here, through the game's gamma table, as the emulated GPU's swap hands over its
// picture. This is Call of Duty 3's cod3_native_vulkan (recomp/src/game/ngl_side.cpp: StartNativeVulkan, Show,
// ShowOnGpu), for Call of Duty 2's two programs.
//
// Two ways to the window:
//  - on the GPU (the usual one): ngl_vk draws on the window's own Vulkan device (ngl_vk_init_on_device), and each
//    picture is drawn into the window's picture on that device (ngl_vk_output); nothing goes through the CPU.
//  - through the CPU, when ngl_vk can't use the window's device: ngl_vk draws on a device of its own, the picture is
//    read back, put through the gamma table and copied up into the window's picture.

#pragma once

#include <cstdint>

#include <rex/runtime.h>  // rex::RuntimeConfig

namespace rex::ui {
class Window;
}

namespace cod2::native {

// From Cod2App::OnPreSetup, before the graphics start: when cod2_native_vulkan is on and this build and PC can draw
// natively, loads the GPU plugin's host backend into config.graphics and returns true. Otherwise changes nothing and
// returns false (the GPU emulation draws, as before).
bool StartNativeVulkan(rex::RuntimeConfig& config);

// True when StartNativeVulkan turned the native picture on for this run.
bool NativeVulkanOn();

// Starts ngl_vk for a width x height picture: on the window's own Vulkan device when it can, else on a device of its
// own. Returns 0 when ngl_vk is ready (as ngl_vk_init does).
int StartNglVkForWindow(uint32_t width, uint32_t height);

// True when ngl_vk draws on the window's device (StartNglVkForWindow), so the picture goes to the window on the GPU.
bool PictureOnGpu();

// Hands ngl_vk's last picture to the window (on the GPU), through table (each channel's 256 values, as shown).
void ShowOnGpu(const uint8_t table[3][256]);

// Hands a width x height picture (4 bytes a pixel: red, green, blue, unused) to the window through the CPU, through
// table (as ShowOnGpu).
void ShowPixels(const uint8_t* rgba, uint32_t width, uint32_t height, const uint8_t table[3][256]);

// [cod2-native] 2026-11-24 g10 (owner: draw the required resolution natively, with no upscaling then downscaling):
// the window whose size the native picture follows. From Cod2App::OnPostSetup; nullptr (the default) keeps ngl_vk's
// target at the size it started with (the game's own 1280x720).
void SetWindow(rex::ui::Window* window);

// [cod2-native] g10: once a frame, on the drawing thread, between frames (before the frame's first draw): makes
// ngl_vk's target the largest 16:9 picture that fits in the window (the game's views and menus are 16:9), with
// ngl_vk_set_target_size, so the picture is drawn at the size it is shown at and handed to the window pixel for
// pixel. The game's own coordinates stay its 1280x720 screen (ngl_vk_draw_quads scales them to the target; the 3D
// is in clip space). Does nothing when the size hasn't changed, while the window has no size (minimized), or with an
// ngl_vk that has no ngl_vk_set_target_size.
void FollowWindowSize();

}  // namespace cod2::native
