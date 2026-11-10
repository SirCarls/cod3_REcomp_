// cod2 - the frame rate: the guest vblank and the frame limit. [acct2-cod2]
//
// The Xbox 360's Direct3D waits for a vblank before it shows each frame (the
// Swap 0x820A7F98 passes the device's interval to VdSwap). At the video mode's
// 60 Hz that is a hidden 60 fps cap, and a frame just over 16.7 ms waits for
// the next vblank (30 fps for that frame). The port runs the guest vblank at
// 1000 Hz, as Call of Duty 3's does (owner 2026-11-07: every game free of the
// 360's limits), so the wait never holds a frame back by more than a
// millisecond, and paces the frames itself:
//
//   cod2_max_fps  0-1000, default 60: before each Com_Frame (0x8233CEF8) the
//                 host waits until the next frame is due (a sleep, then yields
//                 for the last 1.5 ms). 0: no limit.
//
// Why 60 for now: the player's movement steps by each frame's time (Pmove
// 0x82469C18, at most 66 ms a step), so jump heights and speeds change with
// the frame rate (the PC's "125/250/333 fps jumps"). The fixed movement step
// (cod2_move_hz, PLAN.md decision 5, 11-12) makes them the same at any rate;
// the default then becomes 0. The game's own com_maxfps (campaign default 0,
// 0-1000, in whole milliseconds, so 60 there is 62.5 fps) still applies too.
// The game's clock is real time at every rate (the server's fixed 50 ms
// steps; runs 20261107-190000 to -191442).
//
// Both programs: the campaign's Com_Frame is 0x8233CEF8, cod2mp's 0x82368E00
// (the decompile, dev/waw-decomp/cod2_360/mp). cod2mp's own com_maxfps
// defaults to 85 (11 ms, 90.9 fps), so there the port's 60 is the limit too.

#include "game/frame_rate.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/frame_time.h"

#ifdef _WIN32
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(cod2_max_fps, 60, "cod2/Display",
                     "Frame limit, frames a second (0: none). 60 until the fixed movement step "
                     "keeps jumps the same at any rate")
    .range(0, 1000);

namespace cod2::frame_rate {

namespace {

using Clock = std::chrono::steady_clock;

// Sleeps can wake up to about a millisecond late, so the last stretch before
// a frame is due is waited out by yielding instead.
constexpr auto kSpinMargin = std::chrono::microseconds(1500);

Clock::time_point g_next_frame{};  // when the next frame may start
int g_paced_fps = 0;               // the limit g_next_frame was planned with

#ifdef _WIN32
// A high-resolution waitable timer (Windows 10 1803 and later) wakes within a
// fraction of a millisecond whatever the system timer's tick is; nullptr on
// older Windows, which then yields the whole wait.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
HANDLE LimiterTimer() {
  static const HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
  return timer;
}
#endif

// Sleeps about `d` (WaitUntil yields through the last stretch).
void SleepFor(Clock::duration d) {
#ifdef _WIN32
  if (HANDLE timer = LimiterTimer()) {
    LARGE_INTEGER due;  // relative, in 100 ns units
    due.QuadPart = -std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / 100);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer, INFINITE);
  }
#else
  std::this_thread::sleep_for(d);
#endif
}

void WaitUntil(Clock::time_point deadline) {
  for (;;) {
    const auto now = Clock::now();
    if (now >= deadline) return;
    const auto left = deadline - now;
    if (left > kSpinMargin)
      SleepFor(left - kSpinMargin);
    else
      std::this_thread::yield();
  }
}

// "1000.000000" -> "1000".
std::string ShortNumber(std::string text) {
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  return text;
}

}  // namespace

void ApplyGpuDefaults() {
  static bool logged = false;
  if (!rex::cvar::GetFlagInfo("guest_vblank_rate")) return;
  // Only while nobody chose a rate: a later call finds the source kRuntime and
  // leaves it alone, and a rate from the command line or cod2.toml stays.
  if (rex::cvar::GetFlagSource("guest_vblank_rate") == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName("guest_vblank_rate", "1000");
  if (logged) return;
  logged = true;
  const int max_fps = REXCVAR_GET(cod2_max_fps);
  REXLOG_INFO("cod2: guest vblank {} Hz (the 360's is 60), frame limit {}",
              ShortNumber(std::string(rex::cvar::GetFlagByName("guest_vblank_rate"))),
              max_fps > 0 ? std::to_string(max_fps) + " fps (cod2_max_fps)" : std::string("none"));
}

void BeginFrame() {
  const int max_fps = std::clamp(REXCVAR_GET(cod2_max_fps), 0, 1000);
  if (max_fps <= 0) {
    g_paced_fps = 0;
    g_next_frame = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / max_fps));
  const auto now = Clock::now();
  // Start over (no catch-up burst) on the first frame, after the limit
  // changed, and after a frame that ran more than one period late (a level
  // load, the window dragged).
  if (g_paced_fps != max_fps || g_next_frame == Clock::time_point{} || now > g_next_frame + period)
    g_next_frame = now;
  g_paced_fps = max_fps;
  if (now < g_next_frame) WaitUntil(g_next_frame);
  g_next_frame += period;
}

}  // namespace cod2::frame_rate

#if defined(COD2_MP)

// cod2mp's Com_Frame, once a frame on the game's main thread: the frame
// limit, then the frame (cod2mp has no frame-time test).
REX_EXTERN(__imp__sub_82368E00);
REX_HOOK_RAW(sub_82368E00) {
  cod2::frame_rate::BeginFrame();
  __imp__sub_82368E00(ctx, base);
}

#else

// Com_Frame, once a frame on the game's main thread: the frame limit, then
// the frame itself (timed for cod2_debug_frame_time, game/frame_time.h).
REX_EXTERN(__imp__sub_8233CEF8);
REX_HOOK_RAW(sub_8233CEF8) {
  cod2::frame_rate::BeginFrame();
  const int64_t timed = cod2::frame_time::FrameStart();
  __imp__sub_8233CEF8(ctx, base);
  cod2::frame_time::FrameEnd(timed);
}

#endif  // COD2_MP
