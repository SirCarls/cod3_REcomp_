// cod3 - [claude-9b] the game's physics in fixed steps (cod3_physics_hz, 60 a
// second by default), whatever the frame rate. Campaign only for now.
//
// The campaign runs its whole game frame once per drawn frame: Com_Frame ->
// SV_Frame (0x82518E40) -> 0x825173F8 -> G_RunFrame (0x8256D3E0), with that
// frame's milliseconds (no fixed server rate as in Quake 3). G_RunFrame gives
// the physics update 0x824139B8 the frame's time in seconds (f1 = msec *
// 0.001; it caps it at 0.1 s). That one call runs every physics object
// (0x82413910 -> 0x82411B80 each), the physics world's step (0x8217E998, the
// phys_* library) and the work that follows from them. It is G_RunFrame's
// only caller of it, and nothing on the client side steps the physics.
//
// Above 60 fps those steps get small, and objects behave in ways the console
// never showed: a piece of window glass and boxes hit hard enough to kill the
// player, and a tank can't push a door open (owner, 2026-10-15 and 10-21).
// Player movement is not affected (it measured the same at 60, 144 and an
// uncapped rate: bench c9b-1021 physics_fps).
//
// This hook keeps the frames' time and runs the update in whole steps of
// 1 / cod3_physics_hz seconds: none, one or several in a frame, at most 0.1 s
// worth (the game's own cap; time past it is dropped, as the game drops it).
// A step is taken once three quarters of one is due and the difference is
// carried, so at the rate itself every frame gets exactly one step even though
// the game's milliseconds alternate (16, 17, 17 at 60 fps). Between steps the
// physics objects keep their last position, so above the rate they move in
// 60 Hz steps on screen. 0 turns it off: one step a frame with the frame's
// time, as the game does.

#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(cod3_physics_hz, 60, "cod3/Game",
                     "Physics steps a second, whatever the frame rate (window glass, boxes, doors, "
                     "debris). 0 = one step a frame with the frame's time, as the game does, which "
                     "changes how objects behave above 60 fps")
    .range(0, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr double kMaxFrameTime = 0.1;  // seconds: the update's own cap (0x82001930)

double g_due = 0.0;   // seconds of physics time owed (can be a little below 0)
int g_logged_hz = -1;  // the rate last written to the log

// Counts for the debug line every 10 s of game time.
double g_window_time = 0.0;
uint32_t g_window_frames = 0, g_window_steps = 0;

}  // namespace

// The physics update: f1 = the frame's time in seconds.
REX_EXTERN(__imp__sub_824139B8);
REX_HOOK_RAW(sub_824139B8) {
  const int hz = REXCVAR_GET(cod3_physics_hz);
  if (hz != g_logged_hz) {
    g_logged_hz = hz;
    if (hz > 0)
      REXLOG_INFO("cod3: physics: fixed steps of 1/{} s, whatever the frame rate (cod3_physics_hz)", hz);
    else
      REXLOG_INFO("cod3: physics: one step a frame with the frame's time, as the game does "
                  "(cod3_physics_hz 0)");
    g_due = 0.0;
  }
  const double frame_time = ctx.f1.f64;
  if (hz <= 0 || !(frame_time > 0.0)) {  // off, or a frame with no time: as the game does
    __imp__sub_824139B8(ctx, base);
    return;
  }

  const double step = double(float(1.0 / hz));  // the value the guest's single float holds
  const int max_steps = std::max(1, int(kMaxFrameTime * hz + 0.5));
  g_due += frame_time;
  int steps = 0;
  while (g_due >= 0.75 * step && steps < max_steps) {
    ctx.f1.f64 = step;
    __imp__sub_824139B8(ctx, base);
    g_due -= step;
    ++steps;
  }
  if (steps == max_steps && g_due > step) g_due = 0.0;  // past the cap: dropped, as the game does
  ctx.f1.f64 = frame_time;

  g_window_time += frame_time;
  ++g_window_frames;
  g_window_steps += uint32_t(steps);
  if (g_window_time >= 10.0) {
    REXLOG_DEBUG("cod3: physics: {} steps in {} frames over {:.1f} s ({:.1f} steps a second)",
                 g_window_steps, g_window_frames, g_window_time, g_window_steps / g_window_time);
    g_window_time = 0.0;
    g_window_frames = g_window_steps = 0;
  }
}
