// cod3 - per-frame cost report (see perf_stats.h).

#include "game/perf_stats.h"

#include <chrono>
#include <cstdio>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/diagnostics.h"
#include "game/frame_rate.h"
#include "game/thread_profiler.h"  // [hw-scaling]

#if __has_include(<rex/graphics/gpu_stats.h>)
#include <rex/graphics/gpu_stats.h>
#define COD3_HAS_GPU_STATS 1
#else
#define COD3_HAS_GPU_STATS 0
#endif

REXCVAR_DEFINE_BOOL(cod3_perf_overlay, false, "cod3/Performance",
                    "Show what each frame costs (draws, resolves, GPU waits ...) under the frame "
                    "rate");
REXCVAR_DEFINE_INT32(cod3_perf_log, 0, "cod3/Performance",
                     "Write the per-frame cost report to the log every N seconds (0 = never)")
    .range(0, 3600);

namespace cod3 {

namespace {
constexpr auto kPeriod = std::chrono::seconds(1);
}  // namespace

PerfMonitor& PerfMonitor::Get() {
  static PerfMonitor instance;
  return instance;
}

void PerfMonitor::Tick() {
#if COD3_HAS_GPU_STATS
  namespace gs = rex::graphics::stats;
  using Clock = std::chrono::steady_clock;
  static gs::Snapshot last;
  static Clock::time_point last_time{};
  static Clock::time_point last_log{};

  const auto now = Clock::now();
  if (last_time != Clock::time_point{} && now - last_time < kPeriod) return;
  const gs::Snapshot cur = gs::Read();
  const bool first = last_time == Clock::time_point{};
  const gs::Snapshot prev = last;
  last = cur;
  last_time = now;
  if (first) return;

  auto delta = [&](gs::Counter c) { return double(cur[c] - prev[c]); };
  const double frames = delta(gs::Counter::kFrames);
  PerfReport r;
  if (frames >= 1) {
    const double secs = double(cur.time_us - prev.time_us) / 1e6;
    auto per_frame = [&](gs::Counter c) { return float(delta(c) / frames); };
    const FrameStats fs = FrameRate::Get().Stats();
    r.valid = true;
    r.fps = float(frames / secs);
    r.frame_ms = float(secs * 1000.0 / frames);
    r.game_ms = fs.valid ? fs.busy_ms : 0.0f;
    r.draws = per_frame(gs::Counter::kDraws);
    r.resolves = per_frame(gs::Counter::kResolves);
    r.transfers = per_frame(gs::Counter::kRenderTargetTransfers);
    r.submissions = per_frame(gs::Counter::kSubmissions);
    r.gpu_waits = per_frame(gs::Counter::kGpuWaits);
    r.gpu_wait_ms = per_frame(gs::Counter::kGpuWaitMicros) / 1000.0f;
    r.readbacks = per_frame(gs::Counter::kReadbacks);
    r.readback_kb = per_frame(gs::Counter::kReadbackBytes) / 1024.0f;
    r.queries = per_frame(gs::Counter::kOcclusionQueries);
    r.upload_kb = per_frame(gs::Counter::kUploadBytes) / 1024.0f;
    r.texture_loads = per_frame(gs::Counter::kTextureLoads);
    r.watch_faults = per_frame(gs::Counter::kWatchFaults);
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 2
    // [hw-scaling] the GPU emulation thread's busy time.
    r.gpu_thread_ms = per_frame(gs::Counter::kGpuThreadMicros) / 1000.0f;
    r.submit_ms = per_frame(gs::Counter::kSubmitMicros) / 1000.0f;
    r.shader_ms = per_frame(gs::Counter::kShaderTranslateMicros) / 1000.0f;
    r.shaders = per_frame(gs::Counter::kShaderTranslations);
#endif
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 3
    // [hw-scaling] window presents a second.
    r.presents_per_s = float(delta(gs::Counter::kPresents) / secs);
#endif
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 4
    // [hw-scaling] the graphics card's time a frame (0 counted: not timed).
    if (cur[gs::Counter::kHostGpuMicros] > 0)
      r.host_gpu_ms = per_frame(gs::Counter::kHostGpuMicros) / 1000.0f;
#endif
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 6
    // [hw-scaling] the texture loads' guest memory (SDK 0060, which also
    // counts the loads RequestTextures batches).
    r.texture_load_mb = per_frame(gs::Counter::kTextureLoadBytes) / (1024.0f * 1024.0f);
#endif
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 5
    // [hw-scaling] the card's time by kind (SDK 0058).
    {
      static constexpr gs::Counter kKinds[7] = {
          gs::Counter::kHostGpuDrawMicros,    gs::Counter::kHostGpuTransferMicros,
          gs::Counter::kHostGpuResolveMicros, gs::Counter::kHostGpuTextureMicros,
          gs::Counter::kHostGpuUploadMicros,  gs::Counter::kHostGpuSwapMicros,
          gs::Counter::kHostGpuOtherMicros};
      uint64_t any = 0;
      for (gs::Counter k : kKinds) any += cur[k];
      if (any > 0)
        for (int i = 0; i < 7; ++i) r.host_gpu_kind_ms[i] = per_frame(kKinds[i]) / 1000.0f;
    }
#endif
  }
  // [hw-scaling] what limits the frame rate (game/thread_profiler.h, which
  // samples every thread on its own once a second).
  r.limited_by = ThreadProfiler::Verdict(ThreadProfiler::Get().Report());
  {
    std::lock_guard lock(mutex_);
    report_ = r;
  }

  if (r.valid) diag::RecordPerfLine(Describe(r));
  // cod3_perf_log when set, else every 10 s with verbose logging, 2 s with debug.
  int every = REXCVAR_GET(cod3_perf_log);
  if (every <= 0) {
    const auto detail = diag::GetDetail();
    every = detail == diag::Detail::kDebug ? 2 : detail == diag::Detail::kVerbose ? 10 : 0;
  }
  if (every > 0 && r.valid && now - last_log >= std::chrono::seconds(every)) {
    last_log = now;
    REXLOG_INFO("cod3: perf {}", Describe(r));
  }
#endif
}

PerfReport PerfMonitor::Report() {
  std::lock_guard lock(mutex_);
  return report_;
}

std::string PerfMonitor::Describe(const PerfReport& r) {
  char text[768];
  std::snprintf(text, sizeof(text),
                "%.0f fps, %.2f ms/frame (game thread %.2f ms, GPU thread %.2f ms: submit %.2f, "
                "shaders %.2f ms for %.1f) | per frame: %.0f draws, "
                "%.1f resolves, %.1f transfers, %.1f submits, %.1f GPU waits (%.2f ms), "
                "%.1f readbacks (%.0f KB), %.1f queries, upload %.0f KB, %.1f texture loads, "
                "%.0f watch faults",
                r.fps, r.frame_ms, r.game_ms, r.gpu_thread_ms, r.submit_ms, r.shader_ms, r.shaders,
                r.draws, r.resolves, r.transfers, r.submissions, r.gpu_waits, r.gpu_wait_ms,
                r.readbacks, r.readback_kb, r.queries, r.upload_kb, r.texture_loads,
                r.watch_faults);
  std::string line = text;
  // [hw-scaling] how much guest memory the texture loads read (SDK 0060).
  if (r.texture_load_mb >= 0) {
    char texture_mb[48];
    std::snprintf(texture_mb, sizeof(texture_mb), " (texture loads %.1f MB)", r.texture_load_mb);
    line += texture_mb;
  }
  // [hw-scaling] window presents a second, and what limits the frame rate.
  if (r.presents_per_s > 0) {
    char presents[64];
    std::snprintf(presents, sizeof(presents), " | window presents %.0f a second", r.presents_per_s);
    line += presents;
  }
  if (r.host_gpu_ms >= 0) {
    char card[64];
    std::snprintf(card, sizeof(card), " | graphics card %.2f ms a frame", r.host_gpu_ms);
    line += card;
    if (r.host_gpu_kind_ms[0] >= 0) {
      char kinds[192];
      std::snprintf(kinds, sizeof(kinds),
                    " (draws %.2f, transfers %.2f, resolves %.2f, textures %.2f, uploads %.2f, "
                    "swap %.2f, other %.2f)",
                    r.host_gpu_kind_ms[0], r.host_gpu_kind_ms[1], r.host_gpu_kind_ms[2],
                    r.host_gpu_kind_ms[3], r.host_gpu_kind_ms[4], r.host_gpu_kind_ms[5],
                    r.host_gpu_kind_ms[6]);
      line += kinds;
    }
  }
  if (!r.limited_by.empty()) line += " | " + r.limited_by;
  return line;
}

}  // namespace cod3
