// cod3 - [hw-scaling] the game's waits for the GPU emulation sleep (see
// gpu_wait.h).

#include "game/gpu_wait.h"

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

#if __has_include(<rex/graphics/gpu_progress.h>)
#include <rex/graphics/gpu_progress.h>
#define COD3_HAS_GPU_PROGRESS 1
#else
#define COD3_HAS_GPU_PROGRESS 0
#endif

#if __has_include(<rex/thread/thread_stats.h>)
#include <rex/thread/thread_stats.h>
#define COD3_HAS_THREAD_STATS 1
#else
#define COD3_HAS_THREAD_STATS 0
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "game/thread_profiler.h"

REXCVAR_DEFINE_BOOL(cod3_gpu_wait_sleeps, true, "cod3/Performance",
                    "While the game waits for the graphics emulation, its thread sleeps instead "
                    "of keeping a CPU core busy (off: the game's own spin)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// A wait shorter than this spins as the game's did: it ends sooner than a
// sleep and its wake-up could.
constexpr uint64_t kSpinMicros = 50;
// The longest sleep before checking again without a signal.
constexpr uint32_t kSleepMicros = 200;

uint64_t NowMicros() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The calling thread's current wait (a game thread waits for one thing at a
// time).
thread_local uint64_t t_begin_us = 0;
thread_local uint64_t t_seen = 0;  // progress count before the last check
thread_local uint32_t t_reason = 0;

// How long the game's Direct3D waited, by what for (the thread profiler's
// GuestGpuWait groups), whether it spun or slept.
std::atomic<uint64_t> g_wait_us[size_t(cod3::GuestGpuWait::kCount)];

cod3::GuestGpuWait Group(uint32_t reason) {
  using G = cod3::GuestGpuWait;
  switch (reason) {
    case 1: return G::kRingSpace;
    case 2: return G::kCommandMemory;
    case 6: return G::kFence;
    case 0xA: case 0xC: case 0xE: return G::kLock;
    default: return reason >= 7 && reason <= 0x11 ? G::kRelease : G::kOther;
  }
}

void ReadWaits(uint64_t* microseconds, size_t count) {
  for (size_t i = 0; i < count && i < size_t(cod3::GuestGpuWait::kCount); ++i)
    microseconds[i] = g_wait_us[i].load(std::memory_order_relaxed);
}

struct RegisterReader {
  RegisterReader() { cod3::SetGuestGpuWaitReader(ReadWaits); }
} g_register_reader;

bool Enabled() {
  return COD3_HAS_GPU_PROGRESS && REXCVAR_GET(cod3_gpu_wait_sleeps);
}

void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
  _mm_pause();
  _mm_pause();
  _mm_pause();
#endif
}

}  // namespace

// Starts a wait: D3D_WaitBegin (r3 = the wait's record on the stack, r4 =
// the device, r5 = the reason code). The progress count is read here, before
// the loop's first check, so a signal between that check and a sleep wakes
// the sleep.
REX_EXTERN(__imp__sub_822EBFB8);
REX_HOOK_RAW(sub_822EBFB8) {
  t_reason = ctx.r5.u32;
  __imp__sub_822EBFB8(ctx, base);
  t_begin_us = NowMicros();
#if COD3_HAS_GPU_PROGRESS
  t_seen = rex::graphics::progress::Count();
#endif
}

// Ends a wait: D3D_WaitEnd (r3 = the wait's record). Counts its time by
// reason for the thread profiler.
REX_EXTERN(__imp__sub_822EBFE8);
REX_HOOK_RAW(sub_822EBFE8) {
  if (t_begin_us != 0) {
    g_wait_us[size_t(Group(t_reason))].fetch_add(NowMicros() - t_begin_us,
                                                 std::memory_order_relaxed);
    t_begin_us = 0;
  }
  __imp__sub_822EBFE8(ctx, base);
}

// Between two checks of a wait: the game's own short delay and its give-up
// timer (it returns 0 to stop waiting), after this thread's spin or sleep.
REX_EXTERN(__imp__sub_822EC168);
REX_HOOK_RAW(sub_822EC168) {
#if COD3_HAS_GPU_PROGRESS
  if (Enabled() && t_begin_us != 0) {
    namespace progress = rex::graphics::progress;
    const uint64_t now = NowMicros();
    if (now - t_begin_us < kSpinMicros) {
      CpuRelax();
    } else {
      const uint64_t count = progress::Count();
      if (count != t_seen) {
        // The GPU moved since the last check: check again before sleeping.
        t_seen = count;
      } else {
        t_seen = progress::Wait(t_seen, kSleepMicros);
        const uint64_t slept = NowMicros() - now;
#if COD3_HAS_THREAD_STATS
        rex::thread_stats::AddWait(rex::thread_stats::Wait::kGuestObject, slept);
#endif
      }
    }
  }
#endif
  __imp__sub_822EC168(ctx, base);
}
