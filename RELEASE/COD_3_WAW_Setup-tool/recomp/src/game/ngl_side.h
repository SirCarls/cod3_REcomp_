// cod3 - [acct2-bro] 2026-11-05 the frame's 2D drawn natively by ngl_vk, beside the GPU emulation (a check; see
// ngl_side.cpp and dev/agents/acct2-bro/COD3-NGL-PLAN.md, step 3).

#pragma once

#include <cstdint>

#include <rex/runtime.h>

namespace cod3::ngl_side {

// side124: cod3_native_vulkan, the GPU emulation off. In OnPreSetup, once the GPU plugin's name is settled and before
// anything loads the plugin: loads it with its host backend (the game's GPU commands run, nothing is drawn by them)
// into config.graphics, so the native renderer (ngl_vk) draws every frame and the window shows it. Returns whether it
// did; with the setting off, no plugin, or no ngl_vk in the build, it leaves config alone (said in the log).
bool StartNativeVulkan(rex::RuntimeConfig& config);

// At the start of nglListSend (0x8213DAA0), on the main thread, before the frame's lists go to the render job.
// Does nothing unless cod3_debug_ngl_side is on.
void OnListSend(uint8_t* base);

// side224: asked by nglListSend's hook before OnListSend: whether to wait for the game's render job first
// (cod3_native_effects_whole_list in native mode), so the last list's effect flushes are all in before the next counts.
bool WaitForRenderJobFirst();

// At Direct3D's draws (0x822F3A28 indexed, 0x822F3620 plain), on the render job's threads, before the draw; call_site
// is the draw's return address. Reads Direct3D's device only for a frame after some dumps (see ngl_side.cpp,
// kDevicePointer); otherwise does nothing.
void OnDraw(uint8_t* base, uint32_t call_site);

// side191: right after OnDraw, whether that draw is left out of the game's own picture (a check: one of the sky's
// layers named by cod3_debug_skip_game_sky_layers); false otherwise.
bool SkipGameDraw();

// At the generic surface draw's pass setup (0x82142F20: shader state, object, surface), on the render job's threads,
// before that pass's draws. side91: notes the object, so a draw of the first-person gun can be probed. Does nothing
// unless cod3_debug_ngl_side is on.
// [rexglue-native] 2026-11-10 rn43a: and the shader state (0 when not given), whose value block the pass fills for its
// object: the lit draws that follow keep the values the native side reads (cod3_debug_ngl_side_drawn_values).
void OnPass(uint8_t* base, uint32_t object, uint32_t state = 0);

// side230 [acct2-bro] 2026-11-08: a level finished loading (diag::LevelLoadEnd calls it, on the game's main thread).
// With cod3_native_forget_level, the next native frame forgets every native texture and mesh of the level before.
void OnLevelLoaded();

}  // namespace cod3::ngl_side
