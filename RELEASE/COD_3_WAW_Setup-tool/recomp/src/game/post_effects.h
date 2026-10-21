// cod3 - depth of field and motion blur on or off, in the campaign and in
// multiplayer (owner 2026-10-20 19:06: "find motion blur, and this [depth of
// field]. Make it a top priority."; 19:28: "Please add these as toggles.
// Account for problem and account for them without ruining compatibility";
// 19:29: "needs to be in-game as well"; 19:41: "this needs to be able to
// apply within multiplayer as well").
//
// Both are the game's own post effects, drawn at the end of the scene
// (dev/agents/waw-decomp/COD3_DOF_MOTION_BLUR.md has the whole chain):
//
//   Depth of field: one pass (campaign sub_82149C80, multiplayer
//   sub_82161B28) that blends the picture with a quarter-size blurred copy
//   by depth. The campaign draws it while the player is at least 95% aimed
//   down the sights and something has switched depth of field on (an
//   effect's settings or a level script); multiplayer draws it whenever the
//   map has switched it on and one player plays on this PC.
//
//   Motion blur: frame feedback, post effect type 4 (campaign sub_82149950,
//   multiplayer sub_82161758): each frame keeps part of the last one (a
//   quarter-size copy of it), by an amount the missions' scripts start with a
//   length and fades. Saint-Lo, for one, starts 1 second at 0.6.
//
// The settings, both on by default (the game as it was):
//   cod3_depth_of_field  F2 > Video > Effects, Pause > Options > VIDEO
//   cod3_motion_blur     OPTIONS (game/menu_video.h), the setup tool's Play
//                        page, the ~ console; saved in cod3.toml, and
//                        multiplayer takes the campaign's (cod3mp_app.cpp).
// Off skips that one pass and nothing else, so the picture is the frame the
// game draws whenever the pass doesn't run (depth of field: as while you
// aren't aiming; motion blur: as while no moment runs one). The game's own
// values (what a level or an effect set, what it hands its materials) stay
// as they are, so a level and a saved game behave the same either way, and
// turning a setting back on shows the effect at once. It only changes what is drawn
// on this PC: nothing reaches the network or the other players.
//
// Every resolution: both passes size themselves from the render targets the
// game makes from its video mode (the port remakes them at a live
// resolution switch, game/render_resolution.cpp) and take their sampling
// offsets from those textures' own sizes. The one exception is the motion
// blur pass: it treats a 736-line picture as 720 lines (the console's 4:3
// mode is 880x736) and leaves the viewport at 720 lines after it. At any
// other size with 736 lines (a custom resolution) the hook gives the
// viewport its 736 lines back, so what is drawn after it isn't cut short.
//
// cod3_test_post_effects (command line only, off): for test runs, both
// effects are made to show without a level that uses them: depth of field
// with fixed distances (in the campaign drawn by the test too while nobody
// aims, where the game itself only draws it at the sights) and a steady
// motion blur; the game's own values are put back after each frame's post
// effects. A line in the log every 10 seconds counts the passes drawn and
// skipped and how far the weapon came up to the sights.

#pragma once

namespace cod3::post_effects {

// The settings' names (F2, the in-game menu and the setup tool use them).
inline constexpr const char* kDepthOfField = "cod3_depth_of_field";
inline constexpr const char* kMotionBlur = "cod3_motion_blur";

// The settings as they are now (any thread).
bool DepthOfFieldOn();
bool MotionBlurOn();

}  // namespace cod3::post_effects
