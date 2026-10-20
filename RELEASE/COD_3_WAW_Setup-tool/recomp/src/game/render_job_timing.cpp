// cod3 - [hw-scaling] the game's render job, timed (see render_job_timing.h).

#include "game/render_job_timing.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <ctime>
#include <pthread.h>
#endif

#include "game/thread_profiler.h"

namespace {

uint64_t NowMicros() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The calling thread's CPU time, microseconds.
uint64_t ThreadCpuMicros() {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  const uint64_t k = (uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
  const uint64_t u = (uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
  return (k + u) / 10;  // 100 ns units
#else
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return uint64_t(ts.tv_sec) * 1000000u + uint64_t(ts.tv_nsec) / 1000u;
#endif
}

std::atomic<uint64_t> g_jobs{0}, g_job_wall_us{0}, g_job_cpu_us{0};
std::atomic<uint64_t> g_back_end_wall_us{0}, g_back_end_cpu_us{0};
std::atomic<uint64_t> g_swaps_in_job{0}, g_swaps_elsewhere{0};
std::atomic<bool> g_logged_elsewhere{false};

// Inside the render job on this thread (Swap tells its callers apart).
thread_local int t_in_job = 0;

void ReadTotals(cod3::RenderJobTotals* out) {
  out->jobs = g_jobs.load(std::memory_order_relaxed);
  out->job_wall_us = g_job_wall_us.load(std::memory_order_relaxed);
  out->job_cpu_us = g_job_cpu_us.load(std::memory_order_relaxed);
  out->back_end_wall_us = g_back_end_wall_us.load(std::memory_order_relaxed);
  out->back_end_cpu_us = g_back_end_cpu_us.load(std::memory_order_relaxed);
  out->swaps_in_job = g_swaps_in_job.load(std::memory_order_relaxed);
  out->swaps_elsewhere = g_swaps_elsewhere.load(std::memory_order_relaxed);
}

struct RegisterReader {
  RegisterReader() { cod3::SetRenderJobReader(ReadTotals); }
} g_register_reader;

std::string ThreadName() {
#if defined(_WIN32)
  return "thread " + std::to_string(GetCurrentThreadId());
#else
  char name[32] = {};
  pthread_getname_np(pthread_self(), name, sizeof(name));
  return name;
#endif
}

}  // namespace

// The render job: one a frame, on a game job worker.
REX_EXTERN(__imp__sub_82130980);
REX_HOOK_RAW(sub_82130980) {
  const uint64_t wall0 = NowMicros(), cpu0 = ThreadCpuMicros();
  ++t_in_job;
  __imp__sub_82130980(ctx, base);
  --t_in_job;
  g_job_cpu_us.fetch_add(ThreadCpuMicros() - cpu0, std::memory_order_relaxed);
  g_job_wall_us.fetch_add(NowMicros() - wall0, std::memory_order_relaxed);
  g_jobs.fetch_add(1, std::memory_order_relaxed);
}

// Its back end: every node's Render, through Direct3D.
REX_EXTERN(__imp__sub_8214F770);
REX_HOOK_RAW(sub_8214F770) {
  const uint64_t wall0 = NowMicros(), cpu0 = ThreadCpuMicros();
  __imp__sub_8214F770(ctx, base);
  g_back_end_cpu_us.fetch_add(ThreadCpuMicros() - cpu0, std::memory_order_relaxed);
  g_back_end_wall_us.fetch_add(NowMicros() - wall0, std::memory_order_relaxed);
}

// Direct3D's Swap: from the render job, or from elsewhere.
REX_EXTERN(__imp__sub_822F4C28);
REX_HOOK_RAW(sub_822F4C28) {
  if (t_in_job > 0) {
    g_swaps_in_job.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_swaps_elsewhere.fetch_add(1, std::memory_order_relaxed);
    if (!g_logged_elsewhere.exchange(true))
      REXLOG_INFO("cod3: render job timing: Direct3D's Swap called outside the render job, by {} "
                  "(counted from now on)",
                  ThreadName());
  }
  __imp__sub_822F4C28(ctx, base);
}
