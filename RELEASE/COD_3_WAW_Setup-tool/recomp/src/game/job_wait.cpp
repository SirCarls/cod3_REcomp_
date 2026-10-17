// cod3 - [hw-scaling] the game's wait for its own jobs sleeps (see
// job_wait.h).

#include "game/job_wait.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>

#if __has_include(<rex/thread/thread_stats.h>)
#include <rex/thread/thread_stats.h>
#define COD3_HAS_THREAD_STATS 1
#else
#define COD3_HAS_THREAD_STATS 0
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "game/addresses.h"
#include "game/guest.h"
#include "game/machine_profile.h"
#include "game/program.h"

REXCVAR_DEFINE_BOOL(cod3_job_wait_sleeps, true, "cod3/Performance",
                    "While the game waits for its own jobs (the render job, every frame), its "
                    "thread sleeps until the job queue changes instead of checking it over and "
                    "over (off: the game's own loop)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using cod3::guest::ReadU32;

constexpr uint32_t kQueueLock = COD3_ADDR(0x8290E920, 0x82A8B830);    // HANDLE: the queue's mutex (CreateMutex)
constexpr uint32_t kHelperState = COD3_ADDR(0x8290E970, 0x82A8B880);  // what 0x8212A520 runs with when helping
// kQueue (cod3::addr::jobs): +4 jobs queued so far, +8 the group table (32
// bytes a group, +0 its jobs not yet done), +24 jobs finished so far.

// Keep looking without a pause this long first (only with cores to spare).
constexpr uint64_t kSpinMicros = 30;
// The longest sleep before looking again without a change.
constexpr uint32_t kSleepMicros = 500;

// Releases of the queue's lock, all threads, and the calling thread's own.
std::atomic<uint64_t> g_releases{0};
thread_local uint64_t t_own_releases = 0;
std::atomic<uint32_t> g_sleepers{0};
std::mutex g_mutex;
std::condition_variable g_changed;

uint64_t OthersReleases() {
  return g_releases.load(std::memory_order_seq_cst) - t_own_releases;
}

uint64_t NowMicros() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool SpinAllowed() {
  static const bool allowed = cod3::machine::Budget(cod3::machine::Get()).spinning_allowed;
  return allowed;
}

// Jobs of `group` not done yet; all jobs for -1 (0x8212A110's own test).
uint32_t Remaining(uint8_t* base, uint32_t group) {
  const uint32_t queue = cod3::addr::jobs::kQueue;
  if (group == 0xFFFFFFFFu) return ReadU32(base, queue + 4) - ReadU32(base, queue + 24);
  return ReadU32(base, ReadU32(base, queue + 8) + (group << 5));
}

// Waits until another thread releases the queue's lock after `seen` (from
// OthersReleases()), or kSleepMicros.
void WaitForChange(uint64_t seen) {
  if (OthersReleases() != seen) return;
  const uint64_t start = NowMicros();
  if (SpinAllowed()) {
    for (uint32_t i = 1;; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#endif
      if (OthersReleases() != seen) return;
      if ((i & 63) == 0 && NowMicros() - start >= kSpinMicros) break;
    }
  }
  // Sequentially consistent on both sides (here and in the release hook):
  // either this sees the release, or the releaser sees this sleeper.
  g_sleepers.fetch_add(1, std::memory_order_seq_cst);
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_changed.wait_for(lock, std::chrono::microseconds(kSleepMicros),
                       [seen] { return OthersReleases() != seen; });
  }
  g_sleepers.fetch_sub(1, std::memory_order_relaxed);
#if COD3_HAS_THREAD_STATS
  rex::thread_stats::AddWait(rex::thread_stats::Wait::kGuestObject, NowMicros() - start);
#endif
}

}  // namespace

// ReleaseMutex(HANDLE): the game's own, then, for the queue's lock, a note
// that the queue may have changed.
REX_EXTERN(__imp__sub_82344FC0);
REX_HOOK_RAW(sub_82344FC0) {
  const uint32_t handle = ctx.r3.u32;
  __imp__sub_82344FC0(ctx, base);
  if (handle == 0 || handle != ReadU32(base, kQueueLock)) return;
  ++t_own_releases;
  g_releases.fetch_add(1, std::memory_order_seq_cst);
  if (g_sleepers.load(std::memory_order_seq_cst) != 0) {
    { std::lock_guard<std::mutex> lock(g_mutex); }
    g_changed.notify_all();
  }
}

// Waits for the jobs of group r3 (-1 = all): 0x8212A110's loop, with a pause
// when there was nothing to help with.
REX_EXTERN(__imp__sub_8212A110);
REX_EXTERN(sub_8212A520);
REX_HOOK_RAW(sub_8212A110) {
  if (!REXCVAR_GET(cod3_job_wait_sleeps)) {
    __imp__sub_8212A110(ctx, base);
    return;
  }
  const uint64_t group = ctx.r3.u64;
  const uint32_t finished_jobs = cod3::addr::jobs::kQueue + 24;
  for (;;) {
    if (Remaining(base, uint32_t(group)) == 0) return;
    const uint64_t seen = OthersReleases();
    const uint32_t finished = ReadU32(base, finished_jobs);
    // Help: any runnable job, then one of this group (as the game does;
    // multiplayer's wait helps with the group's jobs only [mp]).
    if constexpr (!cod3::kMultiplayer) {
      ctx.r5.s64 = 1;
      ctx.r4.s64 = 0;
      ctx.r3.s64 = int32_t(kHelperState);
      sub_8212A520(ctx, base);
    }
    ctx.r5.s64 = 1;
    ctx.r4.u64 = group;
    ctx.r3.s64 = int32_t(kHelperState);
    sub_8212A520(ctx, base);
    if (Remaining(base, uint32_t(group)) == 0) return;
    // Jobs finished meanwhile (this thread's or others'): ones that waited
    // for them may be runnable now; look again at once.
    if (ReadU32(base, finished_jobs) != finished) continue;
    // Nothing to help with: the rest runs on the workers (or waits for a job
    // they run). Sleep until one of them touches the queue.
    WaitForChange(seen);
  }
}
