// cod3mp: [claude-9b] 2026-10-27 the host's game in fixed updates, as the campaign's server step
// (game/server_step.cpp; owner 2026-10-27 04:26: "a lot of these changes will be ported to MP. Please make sure that's
// accounted for"). dev/agents/claude-9b-think/FINDINGS-MP-PORT.md row 1.
//
// A cod3mp host runs SV_Frame (0x82535810) once per drawn frame with that frame's milliseconds, exactly as the campaign
// did: its game update (G_RunFrame 0x82591708 through SV_RunGameFrame: players' moves, physics from the post-frame step
// 0x82534DB0, grenades, movers, vehicles, script waits, the animation queue) ran about 144 times a second at 144 fps, in
// 7 ms steps, and the snapshots went out every frame. Non-host clients draw interpolated snapshots and run no game
// update, so only a host (a local or split-screen match, or hosting system link) changes.
//
// With cod3mp_server_hz N the frame's milliseconds go into a game clock and SV_Frame runs once per 1/N s step that
// clock passed (none, one or several a frame, at most 6), each with the step's milliseconds. As in the campaign:
//  - The game systems Com_Frame (0x82557D60) runs before SV_Frame (0x8253AB98 three times, 0x824AFFA8, 0x82430278,
//    0x82537EA8, 0x8254D098, 0x8256FFC8) run once per update with the update's time, from the first one's hook, and
//    the frame's own calls are left out (by their return addresses in Com_Frame) (cod3mp_server_step_systems).
//  - Only the frame's last update sends the snapshots; a skipped send still clears "a game frame ran and isn't sent"
//    (0x82B3FE34), or the next update would run without its game frame (the campaign's 30 fps half speed).
//  - Between two updates of one frame the job group is waited for (0x82141E50).
//  - The frame time block (0x825FEBD8: +4 msec, +8 seconds, +12 a constant / msec) holds each update's time while its
//    systems run and the frame's own after; the server's frame msec the client reads (CL_AdjustAngles turning,
//    0x82C2EBE8) gets the frame's own milliseconds back.
// Only while this machine hosts: SV_Frame moves svs.time (0x82B3FE30) only then; until it has, and on a client, every
// frame runs as the game does. Off (0) until the tester's runs pass, then 60.

#include <algorithm>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_INT32(cod3mp_server_hz, 0, "cod3/Game",
                     "Multiplayer host: the game (players' moves, physics, grenades, vehicles, scripts) in updates "
                     "of 1/N s whatever the frame rate, as the console's 60; 0: once per drawn frame with that "
                     "frame's time, as the game does. Clients draw from the host's snapshots either way")
    .range(0, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3mp_server_step_systems, true, "cod3/Game",
                    "With cod3mp_server_hz, the game's systems that run before each game update (animation, "
                    "sound events ...) run with the updates, not in every drawn frame")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3mp_debug_server_step, false, "cod3",
                    "Log once a second the host's game updates against the drawn frames (cod3mp_server_hz)");

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFrameTime = 0x825FEBD8;        // Com_Frame's time block: +4 msec, +8 seconds, +12 K / msec
constexpr uint32_t kFrameTimeK = 0x82681E4C;       // K
constexpr uint32_t kSvTime = 0x82B3FE30;           // svs.time
constexpr uint32_t kGameFramePending = 0x82B3FE34;  // set by SV_Frame's game frame, cleared by the send
constexpr uint32_t kSvFrameMsec = 0x82C2EBE8;      // the server frame's msec the client reads (+4: 0)
constexpr uint32_t kSystemArgs[3] = {0x825FEC6C, 0x825FEC9C, 0x825FED2C};  // 0x8253AB98's r3, in order
constexpr uint32_t kArg824AFFA8 = 0x82BCC7E8;      // *(it): 0x824AFFA8's r3
constexpr uint32_t kArg82430278 = 0x82BCCA44;      // *(it): 0x82430278's r3
constexpr uint32_t kArg82537EA8 = 0x82BCBE78;      // *(it): 0x82537EA8's r3
// Com_Frame's calls, by their return address.
constexpr uint32_t kRetSystem1 = 0x82557FDC, kRetSystem2 = 0x82557FF0, kRetSystem3 = 0x82558004;
constexpr uint32_t kRet824AFFA8 = 0x82558030, kRet82430278 = 0x82558040, kRet82537EA8 = 0x82558060;
constexpr uint32_t kRet8254D098 = 0x825580B4, kRet8256FFC8 = 0x825580B8, kRetSvFrame = 0x82558160;
constexpr int kMaxTicksPerFrame = 6;

int64_t TickEnd(int64_t k, int hz) { return k * 1000 / hz; }
int64_t TickAtOrBefore(int64_t t, int hz) { return ((t + 1) * hz - 1) / 1000; }

struct StepClock {
  int hz = -1;
  int64_t time = 0;
  int64_t tick = 0;
} g_clock;

bool g_hosting = false;    // the last SV_Frame moved svs.time
bool g_block_ran = false;  // this frame's systems and SV_Frame already ran (in the first system's hook)
bool g_skip_send = false;  // an update that isn't the frame's last: no snapshots

struct Counts {
  uint64_t frames = 0, updates = 0, skipped_sends = 0, multi = 0, none = 0;
  Clock::time_point since = Clock::now();
} g_counts;

int Hz() { return REXCVAR_GET(cod3mp_server_hz); }

void ClockChange(int hz) {
  if (hz == g_clock.hz) return;
  if (hz > 0)
    REXLOG_INFO("cod3mp: server step: the host's game in updates of 1/{} s, whatever the frame rate "
                "(cod3mp_server_hz)", hz);
  else if (g_clock.hz > 0)
    REXLOG_INFO("cod3mp: server step: one game update a frame with the frame's time, as the game does");
  g_clock = StepClock{};
  g_clock.hz = hz;
}

void DebugLine() {
  if (!REXCVAR_GET(cod3mp_debug_server_step)) return;
  const auto now = Clock::now();
  const double s = std::chrono::duration<double>(now - g_counts.since).count();
  if (s < 1.0) return;
  REXLOG_INFO("cod3mp: server step: {} frames, {} game updates ({} frames with none, {} with several), {} sends "
              "skipped over {:.2f} s; hosting {}, {} Hz",
              g_counts.frames, g_counts.updates, g_counts.none, g_counts.multi, g_counts.skipped_sends, s,
              g_hosting ? "yes" : "no", g_clock.hz);
  g_counts = Counts{};
}

}  // namespace

REX_EXTERN(__imp__sub_8253AB98);
REX_EXTERN(__imp__sub_824AFFA8);
REX_EXTERN(__imp__sub_82430278);
REX_EXTERN(__imp__sub_82537EA8);
REX_EXTERN(__imp__sub_8254D098);
REX_EXTERN(__imp__sub_8256FFC8);
REX_EXTERN(__imp__sub_82535810);  // SV_Frame(msec)
REX_EXTERN(__imp__sub_825334E8);  // SV_SendClientMessages
REX_EXTERN(sub_82141E50);         // the job group wait (-1: all)

namespace {

void WaitForJobs(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64;
  ctx.r3.s64 = -1;
  sub_82141E50(ctx, base);
  ctx.r3.u64 = r3;
}

// One game update's systems, as Com_Frame calls them, with the update's time (f1 = seconds).
void RunSystems(PPCContext& ctx, uint8_t* base, int tick_msec) {
  const float dt = float(tick_msec) * 0.001f;
  g::WriteU32(base, kFrameTime + 4, uint32_t(std::max(tick_msec, 1)));
  g::WriteF32(base, kFrameTime + 8, dt);
  g::WriteF32(base, kFrameTime + 12, g::ReadF32(base, kFrameTimeK) / float(std::max(tick_msec, 1)));
  for (uint32_t arg : kSystemArgs) {
    ctx.r3.u64 = arg;
    ctx.r5.u64 = 0;
    ctx.f1.f64 = dt;
    __imp__sub_8253AB98(ctx, base);
  }
  ctx.r3.u64 = g::ReadU32(base, kArg824AFFA8);
  ctx.f1.f64 = dt;
  __imp__sub_824AFFA8(ctx, base);
  ctx.r3.u64 = g::ReadU32(base, kArg82430278);
  ctx.f1.f64 = dt;
  __imp__sub_82430278(ctx, base);
  ctx.r3.u64 = g::ReadU32(base, kArg82537EA8);
  ctx.f1.f64 = dt;
  __imp__sub_82537EA8(ctx, base);
  __imp__sub_8254D098(ctx, base);
  __imp__sub_8256FFC8(ctx, base);
}

// The frame's game updates (hz > 0, msec > 0, hosting).
void GameUpdates(PPCContext& ctx, uint8_t* base, int hz, int msec, bool systems) {
  const uint32_t frame_msec = g::ReadU32(base, kFrameTime + 4);
  const float frame_dt = g::ReadF32(base, kFrameTime + 8);
  const float frame_inv = g::ReadF32(base, kFrameTime + 12);
  const uint32_t sv_time_before = g::ReadU32(base, kSvTime);
  g_clock.time += msec;
  const int64_t due = TickAtOrBefore(g_clock.time, hz);
  int64_t n = due - g_clock.tick;
  const bool long_frame = n > kMaxTicksPerFrame;
  if (long_frame) n = 1;  // a map load, a long hitch: one update with all of its time, as the game
  for (int64_t i = 0; i < n; ++i) {
    if (i > 0) WaitForJobs(ctx, base);
    const int tick_msec = static_cast<int>(long_frame ? TickEnd(due, hz) - TickEnd(g_clock.tick, hz)
                                                      : TickEnd(g_clock.tick + 1, hz) - TickEnd(g_clock.tick, hz));
    g_skip_send = !long_frame && i + 1 < n;
    if (systems) RunSystems(ctx, base, tick_msec);
    ctx.r3.s64 = tick_msec;
    __imp__sub_82535810(ctx, base);
    g_skip_send = false;
    ++g_counts.updates;
    if (long_frame) {
      g_clock.tick = due;
      break;
    }
    ++g_clock.tick;
  }
  if (systems) {
    g::WriteU32(base, kFrameTime + 4, frame_msec);
    g::WriteF32(base, kFrameTime + 8, frame_dt);
    g::WriteF32(base, kFrameTime + 12, frame_inv);
  }
  g::WriteU32(base, kSvFrameMsec, static_cast<uint32_t>(msec));
  g::WriteU32(base, kSvFrameMsec + 4, 0);
  if (n > 0) g_hosting = g::ReadU32(base, kSvTime) != sv_time_before;  // the server stopped: as the game again
  ++g_counts.frames;
  if (n == 0) ++g_counts.none;
  if (n > 1) ++g_counts.multi;
  DebugLine();
}

bool Stepping(int hz, int msec) { return hz > 0 && msec > 0 && g_hosting; }

// One of the frame's other systems: already run in the updates when g_block_ran (Com_Frame's call only).
bool SkipSystem(const PPCContext& ctx, uint32_t ret) { return g_block_ran && static_cast<uint32_t>(ctx.lr) == ret; }

}  // namespace

// 0x8253AB98(block, seconds, 0): the first of Com_Frame's systems (its first call: the frame's updates run from here
// with cod3mp_server_hz); the second and third calls are left out when they ran in the updates.
REX_HOOK_RAW(sub_8253AB98) {
  const uint32_t ret = static_cast<uint32_t>(ctx.lr);
  if (ret == kRetSystem1) {
    g_block_ran = false;
    const int hz = Hz();
    ClockChange(hz);
    const int msec = ctx.r29.s32;  // Com_Frame's msec (it passes the same to SV_Frame)
    if (Stepping(hz, msec) && REXCVAR_GET(cod3mp_server_step_systems)) {
      GameUpdates(ctx, base, hz, msec, true);
      g_block_ran = true;
      return;
    }
  } else if (g_block_ran && (ret == kRetSystem2 || ret == kRetSystem3)) {
    return;
  }
  __imp__sub_8253AB98(ctx, base);
}

REX_HOOK_RAW(sub_824AFFA8) {
  if (SkipSystem(ctx, kRet824AFFA8)) return;
  __imp__sub_824AFFA8(ctx, base);
}
REX_HOOK_RAW(sub_82430278) {
  if (SkipSystem(ctx, kRet82430278)) return;
  __imp__sub_82430278(ctx, base);
}
REX_HOOK_RAW(sub_82537EA8) {
  if (SkipSystem(ctx, kRet82537EA8)) return;
  __imp__sub_82537EA8(ctx, base);
}
REX_HOOK_RAW(sub_8254D098) {
  if (SkipSystem(ctx, kRet8254D098)) return;
  __imp__sub_8254D098(ctx, base);
}
REX_HOOK_RAW(sub_8256FFC8) {
  if (SkipSystem(ctx, kRet8256FFC8)) return;
  __imp__sub_8256FFC8(ctx, base);
}

// SV_Frame(msec): already run in the updates (Com_Frame's call), or stepped here (systems off), or as the game.
REX_HOOK_RAW(sub_82535810) {
  const bool from_frame = static_cast<uint32_t>(ctx.lr) == kRetSvFrame;
  if (from_frame && g_block_ran) {
    g_block_ran = false;
    return;
  }
  const int hz = Hz();
  const int msec = ctx.r3.s32;
  if (from_frame && Stepping(hz, msec)) {
    ClockChange(hz);
    GameUpdates(ctx, base, hz, msec, false);
    return;
  }
  const uint32_t sv_time_before = g::ReadU32(base, kSvTime);
  __imp__sub_82535810(ctx, base);
  if (from_frame) {
    g_hosting = g::ReadU32(base, kSvTime) != sv_time_before;
    if (hz > 0) {
      ++g_counts.frames;
      ++g_counts.updates;
      DebugLine();
    }
  }
}

// SV_SendClientMessages: with several updates in one frame only the last one sends.
REX_HOOK_RAW(sub_825334E8) {
  if (g_skip_send) {
    ++g_counts.skipped_sends;
    g::WriteU32(base, kGameFramePending, 0);
    return;
  }
  __imp__sub_825334E8(ctx, base);
}
