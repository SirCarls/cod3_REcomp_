// cod3 - the settings the game starts with when the player hasn't chosen
// otherwise. Cod3App::OnPreSetup applies them to settings still at their
// built-in value; the F2 window's Reset buttons go back to them.
//
// Settings not listed here default to the SDK's own built-in value.

#pragma once

namespace cod3 {

struct AppDefault {
  const char* cvar;
  const char* value;
};

// Keyboard binds, World at War style (controller button <- keys).
inline constexpr AppDefault kKeybindDefaults[] = {
    {"keybind_right_trigger", "LMB"},         // fire
    {"keybind_left_trigger", "RMB"},          // aim down sights
    {"keybind_a", "Space"},                   // jump
    {"keybind_b", "C"},                       // crouch (hold: prone)
    {"keybind_x", "R,F"},                     // reload / use
    {"keybind_y", "1,2"},                     // switch weapon
    {"keybind_right_shoulder", "G"},          // frag grenade
    {"keybind_left_shoulder", "4"},           // smoke grenade
    {"keybind_rstick_press", "V,MMB"},        // melee
    {"keybind_lstick_press", "Shift"},        // sprint / hold breath (SDK patch 0007; the SDK's F clashed with use)
    {"keybind_back", "Tab"},                  // objectives
    {"keybind_start", "Escape,Return"},       // pause menu
};

// [keybinds] The campaign only (Cod3App, after the table above): binoculars
// (B) and sprint / hold breath (Shift) are keys of their own there
// (game/keybinds/binds.h), so by default no key presses the controller's left
// stick click or D-pad, and Shift+arrows press nothing. cod3mp keeps the table
// above (its L3 is +sprintbreath); cod3.toml leaves these out while they are
// at the campaign's default, so each program keeps its own.
inline constexpr AppDefault kCampaignKeybindDefaults[] = {
    {"keybind_lstick_press", ""}, {"keybind_dpad_up", ""},    {"keybind_dpad_down", ""},
    {"keybind_dpad_left", ""},    {"keybind_dpad_right", ""},
};

// Keyboard and mouse on; raw mouse aim replaces the SDK's mouse-to-stick
// emulation (cod3_raw_mouse defaults to on).
inline constexpr AppDefault kControlDefaults[] = {
    {"mnk_mode", "true"},
    {"mnk_mouse", "false"},
};

// Known-good GPU settings for this title (Xenia compatibility notes for
// 415607E1): reading back resolves stops lens flares glowing through walls.
inline constexpr AppDefault kVideoDefaults[] = {
    {"readback_resolve", "fast"},
    // Keep geometry the CPU hasn't touched on the GPU between frames. On,
    // the SDK's default, every vertex and index buffer the game draws from is
    // copied and uploaded again each frame (10-17 MB a frame); CPU writes are
    // still caught by the write watch either way. Xenia's default is off.
    {"clear_memory_page_state", "false"},
    // [claude-9b] New shaders without freezes (SDK patch 0145): new pipelines
    // are built on the pipeline threads only, with no stand-in built first and
    // no wait for them at each submission; an object that needs one appears a
    // frame or a few later. Frame captures, empty shader cache, 3 minutes each
    // (frames over 33 / 50 / 100 ms after the load, before -> after): forest
    // 25/17/10 -> 4/3/3, nightd 19/17/13 -> 3/3/3, saint_lo 19/17/11 -> 6/4/2,
    // crssrds 36/29/17 -> 6/4/3, mp_merv 4 players 25/15/10 -> 11/3/2; pipelines
    // made on the GPU emulation thread -> 0. A build without 0145 skips these
    // (SetDefault only sets settings that exist).
    {"vulkan_async_pipeline_placeholders", "false"},
    {"vulkan_async_wait_at_submission", "false"},
    {"vulkan_async_depth_only_pipelines", "true"},
    // The SDK's own: a frame with a draw still waiting for its pipeline is
    // shown (the object a frame late) instead of held, since a held frame is
    // itself a freeze (up to 2.9 s at a level start before; none after).
    {"vulkan_async_skip_incomplete_frames", "false"},
};

// The value from the tables above, or nullptr.
inline const char* AppDefaultFor(const char* cvar) {
  auto find = [cvar](const auto& table) -> const char* {
    for (const auto& d : table) {
      const char* a = d.cvar;
      const char* b = cvar;
      while (*a && *a == *b) ++a, ++b;
      if (*a == *b) return d.value;
    }
    return nullptr;
  };
  if (const char* v = find(kKeybindDefaults)) return v;
  if (const char* v = find(kControlDefaults)) return v;
  return find(kVideoDefaults);
}

}  // namespace cod3
