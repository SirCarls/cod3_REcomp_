// codclassic - the frame limit. [acct2-uo]
//
// The 360's Direct3D shows each frame on a vblank, which at the console's 60 Hz is a hidden 60 fps cap.
// The port runs the guest vblank at 1000 Hz (port_defaults.cpp), so that wait no longer holds a frame back,
// and paces the frames here instead:
//
//   codclassic_max_fps  0-1000, default 60. Before each Com_Frame the host waits until the next frame is due
//                       (a sleep, then yields for the last 1.5 ms). 0: no limit.
//
// Why 60 for now: CoD Classic registers no pmove_fixed or pmove_msec (neither program has the strings), so
// the player's movement steps by each frame's whole milliseconds, as on the PC, where jump heights depend on
// the frame rate (the "125/250/333 fps jumps"). The movement code is in the game modules (%s_sp.xdl and
// %s_mp.xdl, in the package), so the fixed movement step comes with them (PLAN step 6); the default then
// becomes 0. The game's own com_maxfps still applies under this limit: the campaign registers it with the
// same default string as logfile and com_statmon ("0", no limit), and Com_Frame turns it into whole
// milliseconds (1000 / com_maxfps), then loops over Com_EventLoop until that much time has passed.
//
// Com_Frame (found by its com_maxfps read and its calls: setjmp on the abort frame, the event loop, then the
// timescale/fixedtime step, the server frame and the client frame): campaign 0x8207F060, multiplayer
// 0x82088560. Both are called once a frame from the main loop.

#include "game/frame_rate.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(codclassic_max_fps, 60, "codclassic/Display",
                     "Frame limit, frames a second (0: none). 60 until the fixed movement step keeps "
                     "jumps the same at any rate")
    .range(0, 1000);

namespace codclassic::frame_rate {

namespace {

using Clock = std::chrono::steady_clock;

// A sleep can wake about a millisecond late, so the last stretch before a frame is due is spent yielding.
constexpr auto kYieldMargin = std::chrono::microseconds(1500);

Clock::time_point g_next_frame{};  // when the next frame may start (main thread only)
int g_planned_fps = 0;             // the limit g_next_frame was planned with

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
// A high-resolution waitable timer (Windows 10 1803 and later) wakes within a fraction of a millisecond,
// whatever the system timer's tick. nullptr on older Windows, which then yields through the whole wait.
HANDLE HighResolutionTimer() {
  static const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                     TIMER_MODIFY_STATE | SYNCHRONIZE);
  return timer;
}
#endif

void SleepAbout(Clock::duration length) {
#ifdef _WIN32
  if (HANDLE timer = HighResolutionTimer()) {
    LARGE_INTEGER due;  // negative: relative, in 100 ns units
    due.QuadPart =
        -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(length).count() / 100);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
  }
#else
  std::this_thread::sleep_for(length);
#endif
}

void WaitUntil(Clock::time_point deadline) {
  for (;;) {
    const auto now = Clock::now();
    if (now >= deadline) return;
    const auto left = deadline - now;
    if (left > kYieldMargin)
      SleepAbout(left - kYieldMargin);
    else
      std::this_thread::yield();
  }
}

}  // namespace

void BeginFrame() {
  const int max_fps = std::clamp(REXCVAR_GET(codclassic_max_fps), 0, 1000);
  if (max_fps != g_planned_fps)
    REXLOG_INFO("codclassic: frame limit {}", max_fps > 0 ? std::to_string(max_fps) + " fps" : "none");
  if (max_fps <= 0) {
    g_planned_fps = 0;
    g_next_frame = {};
    return;
  }
  const auto period =
      std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / max_fps));
  const auto now = Clock::now();
  // Start the schedule over (no burst of catch-up frames) on the first frame, when the limit changes and
  // after a frame that ran more than one period late (a level load, the window dragged).
  if (g_planned_fps != max_fps || g_next_frame == Clock::time_point{} || now > g_next_frame + period)
    g_next_frame = now;
  g_planned_fps = max_fps;
  if (now < g_next_frame) WaitUntil(g_next_frame);
  g_next_frame += period;
}

}  // namespace codclassic::frame_rate

#if defined(CODCLASSIC_MP)
#define CODCLASSIC_COM_FRAME sub_82088560
#else
#define CODCLASSIC_COM_FRAME sub_8207F060
#endif
#define CODCLASSIC_FR_IMP_(name) __imp__##name
#define CODCLASSIC_FR_IMP(name) CODCLASSIC_FR_IMP_(name)

// Com_Frame, once a frame on the game's main thread: the frame limit, then the frame.
REX_EXTERN(CODCLASSIC_FR_IMP(CODCLASSIC_COM_FRAME));
REX_HOOK_RAW(CODCLASSIC_COM_FRAME) {
  codclassic::frame_rate::BeginFrame();
  CODCLASSIC_FR_IMP(CODCLASSIC_COM_FRAME)(ctx, base);
}
