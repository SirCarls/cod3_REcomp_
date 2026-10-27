// cod3 - [claude-9b] 2026-10-27 animation clocks that ran with the frame rate (owner 2026-10-27 03:57: "grass
// animations ... seem to be running fast, same goes for water and various other effects"), both programs.
// dev/agents/claude-9b-think/FINDINGS-ANIMSPEED.md and FINDINGS-WORLDCLOCK.md.
//
// The world's own clock (scene time, material parameter 0x19) is real time at any frame rate. Two others are not:
//  - ngl's fallback scene time (HUD, menus, the front end, the loading screen: scenes without a time of their own) is
//    the vblank count snapshot x 16.67 ms (0x8214F2A0), and the guest's vblank runs at guest_vblank_rate (1000 a
//    second here), so it ran 16.7 times fast. cod3_fix_ui_clock: after BeginNextFrame copies the count into the
//    snapshot, the snapshot is replaced with real time in 1/60 s (1/50 in 50 Hz modes). Only BeginNextFrame writes the
//    snapshot and only 0x8214F2A0 reads it, so nothing else changes (the count itself, which the vblank thread
//    increments, is left alone; nothing paces or waits on it).
//  - Material parameter 0x18 is ngl's frame counter: a shader that animates from it steps once per drawn frame.
//    cod3_fix_frame_param: the material parameter writer's 0x18 entries get a 60 Hz frame number instead (the counter
//    itself, ngl's cache stamp, is left alone). Off until a run shows which materials use it (cod3_debug_anim_clock).

#include "game/anim_clock.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_fix_ui_clock, true, "cod3/Video",
                    "HUD, menu and loading-screen animations at their real speed (the game's fallback animation "
                    "clock counted the emulated display's 1000 Hz vblanks as 60 Hz ones); off: 16.7 times fast, as "
                    "before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_fix_frame_param, false, "cod3/Video",
                    "Materials that animate by the frame number step 60 times a second at any frame rate (material "
                    "parameter 0x18); off: once per drawn frame, as the game does")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_anim_clock, false, "cod3",
                    "Log once a second how far the game's animation clocks moved against real time (the frame "
                    "seconds, the world scene's time, the frame counter, the fallback clock) and how many material "
                    "parameters of the frame-number and time kinds were written");

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

#if defined(COD3_MP)
constexpr uint32_t kFrameCounter = 0x82A8C1A0;     // ngl's frame counter (material parameter 0x18)
constexpr uint32_t kVblankSnapshot = 0x82A8C1A8;   // the vblank count at frame start (fallback scene time)
constexpr uint32_t kFifty = 0x82A8C1D9;            // byte: 50 Hz mode (20 ms a vblank)
constexpr uint32_t kFrameSeconds = 0x825FEBE0;     // Com_Frame's seconds this frame (float)
constexpr uint32_t kWorldListTime = 0x82A869E0;    // the world scene's time, summed by RE_BeginFrame (float)
#else
constexpr uint32_t kFrameCounter = 0x8290F28C;
constexpr uint32_t kVblankSnapshot = 0x8290F294;
constexpr uint32_t kFifty = 0x8290F2C5;
constexpr uint32_t kFrameSeconds = 0x825F4E14;
constexpr uint32_t kWorldListTime = 0x82909D50;
#endif
constexpr uint32_t kParamFrameNumber = 0x18;
constexpr uint32_t kParamTime = 0x19;
constexpr uint32_t kParamList = 0x30;     // the writer's r3 + 0x30: the first entry
constexpr uint32_t kParamNext = 0x0C;     // an entry: the next one,
constexpr uint32_t kParamType = 0x10;     //   its type,
constexpr uint32_t kParamDest = 0x14;     //   where its value goes (a u32)
constexpr int kMaxParams = 256;

const Clock::time_point g_epoch = Clock::now();
std::atomic<uint64_t> g_frame_params{0}, g_time_params{0};

double RealSeconds() { return std::chrono::duration<double>(Clock::now() - g_epoch).count(); }
bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// The debug log's running sums, since its last line.
struct Sums {
  bool started = false;
  double real0 = 0, frame_seconds = 0;
  float world0 = 0;
  uint32_t counter0 = 0, snapshot0 = 0;
  uint64_t frames = 0;
} g_sums;

void DebugLine(uint8_t* base) {
  const double now = RealSeconds();
  const float world = g::ReadF32(base, kWorldListTime);
  const uint32_t counter = g::ReadU32(base, kFrameCounter);
  const uint32_t snapshot = g::ReadU32(base, kVblankSnapshot);
  Sums& s = g_sums;
  if (!s.started) {
    s = Sums{true, now, 0, world, counter, snapshot, 0};
    return;
  }
  ++s.frames;
  s.frame_seconds += g::ReadF32(base, kFrameSeconds);
  if (now - s.real0 < 1.0) return;
  const double real = now - s.real0;
  const double ms_per_vblank = g::ReadU8(base, kFifty) ? 20.0 : 1000.0 / 60.0;
  REXLOG_INFO("cod3: animation clocks over {:.2f} s of real time, {} frames: frame seconds {:.2f}, world scene time "
              "{:.2f}, frame counter +{} ({:.1f} a second), fallback clock {:.2f} s; material parameters a frame: "
              "frame number {:.1f}, time {:.1f}",
              real, s.frames, s.frame_seconds, world - s.world0, counter - s.counter0,
              (counter - s.counter0) / real, (snapshot - s.snapshot0) * ms_per_vblank / 1000.0,
              double(g_frame_params.exchange(0)) / double(s.frames), double(g_time_params.exchange(0)) / double(s.frames));
  s = Sums{true, now, 0, world, counter, snapshot, 0};
}

// After the material parameter writer: its frame-number entries (counted, and with cod3_fix_frame_param written as a
// 60 Hz frame number) and its time entries (counted).
void AfterParams(uint8_t* base, uint32_t ctx_ptr) {
  const bool fix = REXCVAR_GET(cod3_fix_frame_param);
  const bool debug = REXCVAR_GET(cod3_debug_anim_clock);
  if (!(fix || debug) || !Valid(ctx_ptr)) return;
  const uint32_t frame60 = static_cast<uint32_t>(std::floor(RealSeconds() * 60.0));
  uint32_t entry = g::ReadU32(base, ctx_ptr + kParamList);
  for (int i = 0; i < kMaxParams && Valid(entry); ++i, entry = g::ReadU32(base, entry + kParamNext)) {
    const uint32_t type = g::ReadU32(base, entry + kParamType);
    if (type == kParamFrameNumber) {
      if (debug) g_frame_params.fetch_add(1, std::memory_order_relaxed);
      const uint32_t dest = g::ReadU32(base, entry + kParamDest);
      if (fix && Valid(dest)) g::WriteU32(base, dest, frame60);
    } else if (type == kParamTime && debug) {
      g_time_params.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

}  // namespace

namespace cod3::anim_clock {

void AfterBeginNextFrame(uint8_t* base) {
  if (REXCVAR_GET(cod3_fix_ui_clock)) {
    const double hz = g::ReadU8(base, kFifty) ? 50.0 : 60.0;
    g::WriteU32(base, kVblankSnapshot, static_cast<uint32_t>(std::floor(RealSeconds() * hz)));
  }
  if (REXCVAR_GET(cod3_debug_anim_clock)) DebugLine(base);
  else g_sums.started = false;
}

}  // namespace cod3::anim_clock

// The material parameter writer (campaign 0x82141D40, cod3mp 0x82159AF8; r3: the draw's parameter context).
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_82159AF8);
REX_HOOK_RAW(sub_82159AF8) {
  const uint32_t params = ctx.r3.u32;
  __imp__sub_82159AF8(ctx, base);
  AfterParams(base, params);
}
#else
REX_EXTERN(__imp__sub_82141D40);
REX_HOOK_RAW(sub_82141D40) {
  const uint32_t params = ctx.r3.u32;
  __imp__sub_82141D40(ctx, base);
  AfterParams(base, params);
}
#endif
