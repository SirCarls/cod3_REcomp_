// cod3 - [hw-scaling] the game's waits for the GPU emulation sleep (see
// gpu_wait.h).

#include "game/gpu_wait.h"

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

// Starts a wait (r3 = the wait's record on the stack, r4 = the device, r5 =
// which wait). The progress count is read here, before the loop's first
// check, so a signal between that check and a sleep wakes the sleep.
REX_EXTERN(__imp__sub_822EBFB8);
REX_HOOK_RAW(sub_822EBFB8) {
  __imp__sub_822EBFB8(ctx, base);
#if COD3_HAS_GPU_PROGRESS
  t_begin_us = NowMicros();
  t_seen = rex::graphics::progress::Count();
#endif
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
