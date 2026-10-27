// cod3 - the Rust renderer's library (engine/crates/cod3_render; the plan is in
// dev/agents/rust-renderer/: the game's draws taken from the SDK's own command
// processor and drawn by Rust with Vulkan, so the emulated Xbox 360 GPU no
// longer runs).
//
// The library is linked into both programs when the build found Rust for its
// target (recomp/cmake/cod3_render.cmake).
//
// The setting cod3_renderer chooses who draws:
//  - "xenos" (the default): the GPU plugin's emulated Xbox 360 GPU, as always.
//  - "rust": the GPU plugin's "host" backend (SDK patch 0100,
//    rex/graphics/host_renderer.h). The GPU emulation is off; the game still
//    sees its GPU (registers, vblank, fences), and every shader, draw, resolve
//    and swap goes to the Rust library. M1 step 2: it only counts them (a log
//    line every 10 s), so the picture stays black. Without the library in the
//    build nothing gets them, and the picture is black too.
//
// Compiled into both programs (cod3 and cod3mp).

#pragma once

#include <rex/runtime.h>

namespace cod3::render {

// In OnPreSetup, once the GPU plugin's name is settled and before anything
// loads the plugin: with cod3_renderer "rust", loads it with the host backend
// into config.graphics and returns true. Otherwise, or if that fails (said in
// the log; the emulated GPU is used then), leaves config alone and returns
// false.
bool StartHostBackend(rex::RuntimeConfig& config);

// Says in the log whether the Rust renderer library is linked, which ABI it
// speaks and whether the host backend uses it. Call once at start.
void ReportRustRenderer();

}  // namespace cod3::render
