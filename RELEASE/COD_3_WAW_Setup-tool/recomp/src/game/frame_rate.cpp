// cod3 - frame rate limit and counter (see frame_rate.h).

#include "game/frame_rate.h"

#include "game/diagnostics.h"
#include "game/watchdog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>  // PPCRegister, for the mid-asm hook below
#include <rex/logging.h>
#include <rex/ui/window.h>

// The presenter's own words for how it shows frames (SDK patch, see
// patches/README.md); without it the log line points to the presenter's line.
#if __has_include(<rex/ui/present_mode.h>)
#include <rex/ui/present_mode.h>
#define COD3_HAS_PRESENT_MODE 1
#else
#define COD3_HAS_PRESENT_MODE 0
#endif

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

namespace cod3 {

namespace {
// How often the counter's numbers are refreshed (World at War updates its
// cg_drawFPS a few times a second too; faster is unreadable).
constexpr auto kStatsPeriod = std::chrono::milliseconds(500);
// Sleeps can wake up to about a millisecond late, so the last stretch before
// a frame is due is waited out by yielding instead.
constexpr auto kSpinMargin = std::chrono::microseconds(1500);

#ifdef _WIN32
// What SetUpHostTiming got, for the log line.
bool g_timer_1ms = false;          // timeBeginPeriod(1) accepted
bool g_timer_kept_hidden = false;  // Windows told to keep it while the window seems hidden

// The limiter's high-resolution waitable timer (Windows 10 1803 and later):
// it wakes within a fraction of a millisecond whatever the timer tick is.
// Only the guest main thread waits on it. nullptr on older Windows.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  // Windows 10 1803 SDK and later
#endif
HANDLE LimiterTimer() {
  static const HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
  return timer;
}

// The system timer's tick now and its finest, in milliseconds (0 if unknown).
void TimerTick(double* now_ms, double* finest_ms) {
  using QueryFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
  static const auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution")));
  ULONG coarsest = 0, finest = 0, current = 0;  // in 100 ns units
  *now_ms = *finest_ms = 0.0;
  if (query && query(&coarsest, &finest, &current) == 0) {
    *now_ms = current / 10000.0;
    *finest_ms = finest / 10000.0;
  }
}
#endif

// Sleeps about `d` (the caller yields through the last stretch).
void SleepFor(std::chrono::steady_clock::duration d) {
#ifdef _WIN32
  if (HANDLE timer = LimiterTimer()) {
    LARGE_INTEGER due;  // relative, in 100 ns units
    due.QuadPart = -std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / 100);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
      return;
  }
  // No high-resolution timer: Sleep() wakes on the next tick after the time
  // asked (std::this_thread::sleep_for rounds up to whole milliseconds on top
  // of that), so ask for the whole milliseconds only.
  ::Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()));
#else
  std::this_thread::sleep_for(d);
#endif
}

std::string FormatHz(double hz) {
  char text[32];
  if (std::abs(hz - std::round(hz)) < 0.005)
    std::snprintf(text, sizeof(text), "%.0f Hz", hz);
  else
    std::snprintf(text, sizeof(text), "%.2f Hz", hz);
  return text;
}
}  // namespace

void SetUpHostTiming() {
#ifdef _WIN32
  // Sleeps and waits with a time-out wake on the system timer's tick, 15.6 ms
  // unless a program asks for 1 ms (since Windows 10 2004 only for the asking
  // program). The GPU emulation's vblank and command threads wait in 1 ms
  // steps every frame.
  g_timer_1ms = timeBeginPeriod(1) == TIMERR_NOERROR;
  // Windows 11 may ignore that request while the window is minimized, covered
  // or otherwise seems hidden (power throttling), which would hold the game
  // near 32 fps. Microsoft's documented opt-out: always keep it.
  // Declared here so older Windows SDKs build too (processthreadsapi.h:
  // ProcessPowerThrottling = 4, PROCESS_POWER_THROTTLING_CURRENT_VERSION = 1,
  // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION = 0x4).
  struct PowerThrottlingState {
    ULONG version;
    ULONG control_mask;
    ULONG state_mask;
  };
  constexpr int kProcessPowerThrottling = 4;
  using SetInfoFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
  if (const auto set_info = reinterpret_cast<SetInfoFn>(reinterpret_cast<void*>(
          GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")))) {
    PowerThrottlingState state{1, 0x4, 0};  // ignore-timer-resolution: never
    g_timer_kept_hidden =
        set_info(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state)) != FALSE;
  }
  LimiterTimer();  // made here; the handle works on any thread
#endif
}

void LogFramePacingOnce(rex::ui::Window* window) {
  static bool logged = false;
  if (logged) return;
  const FrameStats stats = FrameRate::Get().Stats();
  if (!stats.valid) return;  // the game hasn't run for a moment yet
  std::string present;
#if COD3_HAS_PRESENT_MODE
  present = rex::ui::GetPresentModeDescription();
  if (present.empty()) return;  // no swap chain yet
#else
  present = "see the presenter's line";
#endif
  logged = true;

  // V-Sync as F2, the game's Options > Video and the setup tool set it.
  const bool d3d12 = present.rfind("Direct3D 12", 0) == 0;
  const char* vsync_cvar = d3d12 ? "d3d12_allow_variable_refresh_rate_and_tearing"
                                 : "vulkan_allow_present_mode_immediate";
  const char* vsync = "?";
  if (rex::cvar::GetFlagInfo(vsync_cvar))
    vsync = rex::cvar::GetFlagByName(vsync_cvar) == "true" ? "off" : "on";  // tearing allowed = off

  std::string display = "display refresh unknown";
  if (window) {
    const double hz = window->GetDisplayRefreshRate();
    if (hz > 0.0) display = "display " + FormatHz(hz);
    if (!window->IsFullscreen())
      display += ", windowed";
    else if (window->GetFullscreenRefreshRate() > 0.0)
      display += ", fullscreen at a chosen rate";
    else
      display += ", borderless fullscreen";
  }

#ifdef _WIN32
  double tick_ms = 0.0, finest_ms = 0.0;
  TimerTick(&tick_ms, &finest_ms);
  char timer[160];
  std::snprintf(timer, sizeof(timer), "timer %.2f ms (finest %.2f)%s%s, limiter waits %s",
                tick_ms, finest_ms, g_timer_1ms ? "" : ", 1 ms refused",
                g_timer_kept_hidden ? ", kept when hidden" : "",
                LimiterTimer() ? "high-resolution" : "on the tick");
#else
  const char* timer = "timers precise (nanosleep)";
#endif

  std::string vblank = "?";
  if (rex::cvar::GetFlagInfo("guest_vblank_rate")) {
    vblank = rex::cvar::GetFlagByName("guest_vblank_rate");  // "1000.000000"
    if (vblank.find('.') != std::string::npos) {
      while (vblank.back() == '0') vblank.pop_back();
      if (vblank.back() == '.') vblank.pop_back();
    }
  }
  REXLOG_INFO("cod3:   frame pacing: {} (com_maxfps), {:.0f} fps now; V-Sync {}, present {}; {}; "
              "{}; guest vblank {} Hz",
              stats.limit > 0 ? "limit " + std::to_string(stats.limit) + " fps" : "no limit",
              stats.fps, vsync, present, display, timer, vblank);
}

FrameRate& FrameRate::Get() {
  static FrameRate instance;
  return instance;
}

void FrameRate::WaitUntil(Clock::time_point deadline) {
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

void FrameRate::BeginFrame(int max_fps) {
  limit_.store(std::max(0, max_fps), std::memory_order_relaxed);
  const auto work_end = Clock::now();
  const double busy_s = wait_end_ == Clock::time_point{}
                            ? 0.0
                            : std::chrono::duration<double>(work_end - wait_end_).count();
  if (max_fps > 0) {
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / std::min(max_fps, 1000)));
    auto now = Clock::now();
    // Start over (no catch-up burst) on the first frame, after the limit
    // changed, and after a frame that ran more than one period late.
    if (paced_fps_ != max_fps || next_frame_ == Clock::time_point{} || now > next_frame_ + period)
      next_frame_ = now;
    paced_fps_ = max_fps;
    if (now < next_frame_) WaitUntil(next_frame_);
    next_frame_ += period;
  } else {
    paced_fps_ = 0;
    next_frame_ = {};
  }
  wait_end_ = Clock::now();
  if (last_frame_ != Clock::time_point{})
    diag::OnFrame(std::chrono::duration<double>(wait_end_ - last_frame_).count(), busy_s);
  Record(wait_end_, busy_s);
}

void FrameRate::Record(Clock::time_point now, double busy_s) {
  if (last_frame_ == Clock::time_point{}) {
    last_frame_ = period_start_ = now;
    return;
  }
  const double dt = std::chrono::duration<double>(now - last_frame_).count();
  last_frame_ = now;
  // A long stall (level load, the window dragged) isn't a frame rate.
  if (dt > 3.0) {
    period_start_ = now;
    period_frames_ = 0;
    period_busy_s_ = 0.0;
    return;
  }
  if (period_frames_ == 0) {
    period_min_s_ = period_max_s_ = dt;
  } else {
    period_min_s_ = std::min(period_min_s_, dt);
    period_max_s_ = std::max(period_max_s_, dt);
  }
  period_busy_s_ += busy_s;
  ++period_frames_;
  const auto elapsed = now - period_start_;
  if (elapsed < kStatsPeriod) return;
  const double secs = std::chrono::duration<double>(elapsed).count();
  FrameStats s;
  s.fps = static_cast<float>(period_frames_ / secs);
  s.frame_ms = static_cast<float>(secs * 1000.0 / period_frames_);
  s.min_ms = static_cast<float>(period_min_s_ * 1000.0);
  s.max_ms = static_cast<float>(period_max_s_ * 1000.0);
  s.busy_ms = static_cast<float>(period_busy_s_ * 1000.0 / period_frames_);
  s.valid = true;
  {
    std::lock_guard lock(mutex_);
    published_ = s;
  }
  period_start_ = now;
  period_frames_ = 0;
  period_busy_s_ = 0.0;
}

FrameStats FrameRate::Stats() {
  FrameStats s;
  {
    std::lock_guard lock(mutex_);
    s = published_;
  }
  s.limit = limit();
  return s;
}

}  // namespace cod3

// ---------------------------------------------------------------------------
// Mid-asm hook at the top of Com_Frame's wait (0x82536EE0, config/default.toml):
//   r11 = com_maxfps->integer
//   r30 = minimum milliseconds per frame (the game sets 1000 / com_maxfps)
// The host has already waited, so the game's own loop only needs one
// millisecond to pass between frames.
// ---------------------------------------------------------------------------
void Cod3FrameLimiterHook(PPCRegister& r11, PPCRegister& r30) {
  cod3::watchdog::MarkFrame();  // the hang watchdog (game/watchdog.h)
  const int max_fps = r11.s32;
  cod3::FrameRate::Get().BeginFrame(max_fps);
  if (max_fps > 0) r30.s64 = 1;
}
