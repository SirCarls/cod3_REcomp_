// cod2 - where a frame's time goes (the 500 fps target, owner 2026-11-07 18:59). [acct2-cod2]
//
// Tests only, off by default: with cod2_debug_frame_time=N (seconds), the
// campaign logs every N seconds how long a frame spends, on average, inside a
// few of its functions (host wall time from entry to return):
//
//   main thread    Com_Frame 0x8233CEF8 (the whole frame), Com_EventLoop
//                  0x8233C228 (the frame limit's wait and the events),
//                  SV_Frame 0x8230B270 (the server: entities, AI, scripts),
//                  R_RenderScene 0x8237C968 (the front end: the scene's draw
//                  surfaces and render commands), CG_Draw2D 0x8245BFD0,
//                  FX_DrawAll 0x82474130, 0x8237F7F0 (the main thread's sync
//                  with the render thread) and 0x82334528 (called by it, likely
//                  the wait itself);
//   render thread  RB_ExecuteRenderCommands 0x823744E8 (the back end of a
//                  frame), its draw-surfaces command 0x823713E0 and Direct3D's
//                  Swap 0x820A7F98.
//
// Second set (after the first run, 20261107-192037: a ~14 ms frame at 70 fps
// with R_RenderScene 2.1 ms on the main thread and a 13 ms back end of which
// the draw-surfaces command is 1 ms): Com_Frame's own callees (its try block
// 0x8233CD70) and RB_ExecuteRenderCommands' callees, to find the waits:
//
//   main thread    CL_RunOncePerClientFrame 0x82425BB0, IN_Frame 0x822CA218,
//                  Cbuf_Execute 0x8233F100, and the unnamed callees 0x82422D20,
//                  0x82304ED0, 0x82309508, 0x8233B7E0, 0x8233C008, 0x8233AD80,
//                  0x8233E560, 0x82288F10, 0x8242EF48, 0x8233BA88; the render
//                  commands' allocator 0x8237DD20 and its one callee 0x82289790;
//   render thread  0x820A54B8 (Direct3D's command buffer: room for more, which
//                  can wait for the GPU), the resolve 0x820AD6E0, 0x820A7E28,
//                  0x820A5E78, 0x8237FE40, 0x82365A00, 0x8236D430, 0x8236D0D0,
//                  0x8236CE38.
//
// Some of these run inside others (the scene and the 2D inside the client's
// frame, the sync before the scene), so the parts are not meant to add up.
// Com_Frame's own hook is the frame limit's (frame_rate.cpp): it calls
// FrameStart/FrameEnd (frame_time.h) after the limit's wait, so the wait
// isn't counted in the frame.
// Addresses from the decompile (dev/waw-decomp/cod2_360/sp). The hooks only
// time; the game runs as before.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/frame_time.h"

REXCVAR_DEFINE_INT32(cod2_debug_frame_time, 0, "cod2/Debug",
                     "Tests: every this many seconds, log where a frame's time went (0: off)")
    .range(0, 600);

#if !defined(COD2_MP)

namespace {

enum Part : uint32_t {
  kComFrame,
  kEventLoop,
  kServer,
  kScene,
  kDraw2D,
  kEffects,
  kRenderSync,
  kRenderWait,
  kBackEnd,
  kDrawSurfaces,
  kSwap,
  kClientFrame,
  kInput,
  kCommands,
  k82422D20,
  k82304ED0,
  k82309508,
  k8233B7E0,
  k8233C008,
  k8233AD80,
  k8233E560,
  k82288F10,
  k8242EF48,
  k8233BA88,
  kCommandAlloc,
  k82289790,
  kRingRoom,
  kResolve,
  k820A7E28,
  k820A5E78,
  k8237FE40,
  k82365A00,
  k8236D430,
  k8236D0D0,
  k8236CE38,
  kParts
};

constexpr const char* kPartNames[kParts] = {
    "Com_Frame",
    "Com_EventLoop",
    "SV_Frame",
    "R_RenderScene",
    "CG_Draw2D",
    "FX_DrawAll",
    "sync 0x8237F7F0",
    "wait 0x82334528",
    "back end",
    "draw surfaces",
    "Swap",
    "CL_RunOncePerClientFrame",
    "IN_Frame",
    "Cbuf_Execute",
    "0x82422D20",
    "0x82304ED0",
    "0x82309508",
    "0x8233B7E0",
    "0x8233C008",
    "0x8233AD80",
    "0x8233E560",
    "0x82288F10",
    "0x8242EF48",
    "0x8233BA88",
    "R_GetCommandBuffer 0x8237DD20",
    "0x82289790",
    "D3D room 0x820A54B8",
    "D3D resolve 0x820AD6E0",
    "0x820A7E28",
    "0x820A5E78",
    "0x8237FE40",
    "0x82365A00",
    "0x8236D430",
    "0x8236D0D0",
    "0x8236CE38",
};

struct Timer {
  std::atomic<uint64_t> ns{0};
  std::atomic<uint32_t> calls{0};
};

std::array<Timer, kParts> g_timers;
std::atomic<int64_t> g_window_start_ns{0};

bool On() { return REXCVAR_GET(cod2_debug_frame_time) > 0; }

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Adds the time from its creation to its end to one part, when the test is on.
class Timed {
 public:
  explicit Timed(Part part) : part_(part), start_(On() ? NowNs() : 0) {}
  ~Timed() {
    if (start_ == 0) return;
    Timer& t = g_timers[part_];
    t.ns.fetch_add(static_cast<uint64_t>(NowNs() - start_), std::memory_order_relaxed);
    t.calls.fetch_add(1, std::memory_order_relaxed);
  }
  Timed(const Timed&) = delete;
  Timed& operator=(const Timed&) = delete;

 private:
  Part part_;
  int64_t start_;
};

// After a whole Com_Frame: the log line once the window has passed.
void MaybeLog() {
  const int64_t now = NowNs();
  const int64_t start = g_window_start_ns.load(std::memory_order_relaxed);
  if (start == 0) {
    g_window_start_ns.store(now, std::memory_order_relaxed);
    return;
  }
  const int64_t window = int64_t(REXCVAR_GET(cod2_debug_frame_time)) * 1000000000LL;
  if (now - start < window) return;
  g_window_start_ns.store(now, std::memory_order_relaxed);

  std::array<uint64_t, kParts> ns{};
  std::array<uint32_t, kParts> calls{};
  for (uint32_t i = 0; i < kParts; ++i) {
    ns[i] = g_timers[i].ns.exchange(0, std::memory_order_relaxed);
    calls[i] = g_timers[i].calls.exchange(0, std::memory_order_relaxed);
  }
  const uint32_t frames = calls[kComFrame] ? calls[kComFrame] : 1;
  const double seconds = double(now - start) / 1e9;
  std::string parts;
  for (uint32_t i = 0; i < kParts; ++i) {
    if (calls[i] == 0) continue;
    parts += fmt::format("; {} {:.3f} ms ({:.2f} calls)", kPartNames[i], double(ns[i]) / 1e6 / frames,
                         double(calls[i]) / frames);
  }
  REXLOG_INFO("cod2: frame time: {:.2f} s, {} frames ({:.1f} a second), a frame{}", seconds,
              calls[kComFrame], double(calls[kComFrame]) / seconds, parts);
}

}  // namespace

#define COD2_TIMED_HOOK(addr, part)            \
  REX_EXTERN(__imp__sub_##addr);               \
  REX_HOOK_RAW(sub_##addr) {                   \
    Timed timed(part);                         \
    __imp__sub_##addr(ctx, base);              \
  }

// Com_Frame: its hook is the frame limit's (game/frame_rate.cpp), which
// calls these around the frame; the log line comes after it.
namespace cod2::frame_time {

int64_t FrameStart() { return On() ? NowNs() : 0; }

void FrameEnd(int64_t start) {
  if (start != 0) {
    Timer& t = g_timers[kComFrame];
    t.ns.fetch_add(static_cast<uint64_t>(NowNs() - start), std::memory_order_relaxed);
    t.calls.fetch_add(1, std::memory_order_relaxed);
  }
  if (On()) MaybeLog();
}

}  // namespace cod2::frame_time

COD2_TIMED_HOOK(8233C228, kEventLoop)
COD2_TIMED_HOOK(8230B270, kServer)
COD2_TIMED_HOOK(8237C968, kScene)
COD2_TIMED_HOOK(8245BFD0, kDraw2D)
COD2_TIMED_HOOK(82474130, kEffects)
COD2_TIMED_HOOK(8237F7F0, kRenderSync)
COD2_TIMED_HOOK(82334528, kRenderWait)
COD2_TIMED_HOOK(823744E8, kBackEnd)
COD2_TIMED_HOOK(823713E0, kDrawSurfaces)
COD2_TIMED_HOOK(820A7F98, kSwap)
COD2_TIMED_HOOK(82425BB0, kClientFrame)
COD2_TIMED_HOOK(822CA218, kInput)
COD2_TIMED_HOOK(8233F100, kCommands)
COD2_TIMED_HOOK(82422D20, k82422D20)
COD2_TIMED_HOOK(82304ED0, k82304ED0)
COD2_TIMED_HOOK(82309508, k82309508)
COD2_TIMED_HOOK(8233B7E0, k8233B7E0)
COD2_TIMED_HOOK(8233C008, k8233C008)
COD2_TIMED_HOOK(8233AD80, k8233AD80)
COD2_TIMED_HOOK(8233E560, k8233E560)
COD2_TIMED_HOOK(82288F10, k82288F10)
COD2_TIMED_HOOK(8242EF48, k8242EF48)
COD2_TIMED_HOOK(8233BA88, k8233BA88)
COD2_TIMED_HOOK(8237DD20, kCommandAlloc)
COD2_TIMED_HOOK(82289790, k82289790)
COD2_TIMED_HOOK(820A54B8, kRingRoom)
COD2_TIMED_HOOK(820AD6E0, kResolve)
COD2_TIMED_HOOK(820A7E28, k820A7E28)
COD2_TIMED_HOOK(820A5E78, k820A5E78)
COD2_TIMED_HOOK(8237FE40, k8237FE40)
COD2_TIMED_HOOK(82365A00, k82365A00)
COD2_TIMED_HOOK(8236D430, k8236D430)
COD2_TIMED_HOOK(8236D0D0, k8236D0D0)
COD2_TIMED_HOOK(8236CE38, k8236CE38)

#endif  // !COD2_MP
