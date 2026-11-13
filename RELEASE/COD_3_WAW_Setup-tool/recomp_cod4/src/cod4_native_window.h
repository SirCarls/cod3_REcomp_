// cod4 - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation: the native renderer as the game's own picture,
// with the GPU emulation off (cod4_native_vulkan). [waw-decomp-cod4] 2026-11-11 round 6
//
// Off by default, and in development. When it is on, the GPU plugin's host backend is loaded in place of the GPU
// emulation: it reads and answers the game's GPU commands (registers, fences, interrupts, swaps) as the emulation
// does, but draws nothing. Each frame is drawn by the native side (cod4_native_side.cpp, with ngl_vk) from what the
// game's own back end submits, and handed to the window here, through the game's gamma table, as the emulated GPU's
// swap hands over its picture. Ported from Call of Duty 3's cod3_native_vulkan (recomp/src/game/ngl_side.cpp:
// StartNativeVulkan, Show, ShowOnGpu; rexglue-native's and acct2-bro's) by way of Call of Duty 2's cod2_native_vulkan
// (recomp_cod2/src/game/native_window.h and .cpp, waw-decomp's 4z61), for Call of Duty 4's two programs.
//
// Two ways to the window:
//  - on the GPU (the usual one): ngl_vk draws on the window's own Vulkan device (ngl_vk_init_on_device), and each
//    picture is drawn into the window's picture on that device (ngl_vk_output); nothing goes through the CPU.
//  - through the CPU, when ngl_vk can't use the window's device: ngl_vk draws on a device of its own, the picture is
//    read back, put through the gamma table and copied up into the window's picture.

#pragma once

#include <cstdint>

#include <rex/runtime.h>  // rex::RuntimeConfig

namespace cod4::native {

// From Cod4App::OnPreSetup, before the graphics start: when cod4_native_vulkan is on and this build and PC can draw
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

}  // namespace cod4::native
