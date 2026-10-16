// cod3 - detailed logging and reports (see diagnostics.h).

#include "game/diagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/version.h>

#if __has_include(<rex/graphics/gpu_stats.h>)
#include <rex/graphics/gpu_stats.h>
#define COD3_HAS_GPU_STATS 1
#else
#define COD3_HAS_GPU_STATS 0
#endif

#if !defined(_WIN32)
#include <sys/utsname.h>
#endif

#include "game/hd_textures.h"
#include "game/machine_profile.h"  // [hw-scaling]
#include "game/render_resolution.h"
#include "game/thread_profiler.h"  // [hw-scaling]
#include "util.h"

REXCVAR_DEFINE_STRING(cod3_log_detail, "normal", "cod3/Performance",
                      "How much the game logs: normal, verbose (slow frames, load times, costs "
                      "every 10 s) or debug (also the SDK's debug messages)")
    .allowed({"normal", "verbose", "debug"});

namespace cod3::diag {

namespace {

using Clock = std::chrono::steady_clock;

std::filesystem::path g_logs_dir;
std::mutex g_mutex;  // the histories below

// Slow frames: the last few, kept for the report.
struct SlowFrame {
  std::string when;
  std::string line;
  double ms;
};
std::deque<SlowFrame> g_slow_frames;      // newest last
constexpr size_t kSlowFramesKept = 40;

std::deque<std::string> g_perf_lines;      // one a second, newest last
constexpr size_t kPerfLinesKept = 60;

std::vector<std::string> g_level_loads;    // "saint_lo: 43.2 s (briefing after load 12.0 s)"

// Guest main thread state.
double g_avg_frame_s = 0.0;
Clock::time_point g_last_slow_log{};
int g_slow_suppressed = 0;
#if COD3_HAS_GPU_STATS
rex::graphics::stats::Snapshot g_last_gpu;
#endif

std::string g_load_map;
Clock::time_point g_load_start{};
Clock::time_point g_load_movie{};

std::string Timestamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return buf;
}

std::string OsSummary() {
#if defined(_WIN32)
  return "Windows";
#else
  utsname u{};
  if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release + " " + u.machine;
  return "POSIX";
#endif
}

std::string Flag(const char* name) {
  if (!rex::cvar::GetFlagInfo(name)) return "-";
  std::string v = rex::cvar::GetFlagByName(name);
  if (v.empty()) return "\"\"";
  // 144.000000 -> 144, 5.500000 -> 5.5
  if (v.find('.') != std::string::npos &&
      v.find_first_not_of("0123456789.-") == std::string::npos) {
    while (v.back() == '0') v.pop_back();
    if (v.back() == '.') v.pop_back();
  }
  return v;
}

void ApplyDetail(const std::string& value) {
  // The SDK's own messages: debug only at the highest detail.
  if (rex::cvar::GetFlagInfo("log_level")) {
    const std::string want = value == "debug" ? "debug" : "info";
    if (rex::cvar::GetFlagByName("log_level") != want) rex::cvar::SetFlagByName("log_level", want);
  }
  // Every new resolve destination copied back to the CPU (SDK patch 0012).
  if (rex::cvar::GetFlagInfo("readback_resolve_log"))
    rex::cvar::SetFlagByName("readback_resolve_log", value == "debug" ? "true" : "false");
}

}  // namespace

Detail GetDetail() {
  const std::string& v = REXCVAR_GET(cod3_log_detail);
  if (v == "debug") return Detail::kDebug;
  if (v == "verbose") return Detail::kVerbose;
  return Detail::kNormal;
}

std::string SettingsSummary() {
  const RenderPlan& plan = GetRenderPlan();
  std::string s;
  s += "render: " + (plan.game_width ? DescribePlan(plan) : std::string("original")) +
       " (choice " + Flag("cod3_render_resolution") + ", EDRAM " +
       std::to_string(plan.edram_tiles) + " tiles)\n";
  s += "display: fullscreen " + Flag("fullscreen") + ", refresh " + Flag("cod3_refresh_rate") +
       " Hz (0 = desktop), tearing allowed (no V-Sync) " +
       Flag("vulkan_allow_present_mode_immediate") +
       ", letterbox " + Flag("present_letterbox") + "\n";
  s += "gpu: api " + Flag("cod3_graphics_api") + ", scale " + Flag("resolution_scale") +
       ", native_2x_msaa " + Flag("native_2x_msaa") + ", post " + Flag("swap_post_effect") +
       ", present effect " + Flag("present_effect") + ", readback " + Flag("readback_resolve") +
       ", rt path vk " + Flag("render_target_path_vulkan") + " / d3d12 " +
       Flag("render_target_path_d3d12") + ", vblank " + Flag("guest_vblank_rate") + " Hz\n";
  // The pack's state, not its path: the report leaves paths out (user names).
  s += "textures: HD (waifu2x) " + Flag("cod3_hd_textures") + ", " +
       DescribeHdTextures(GetHdTextures()) + "\n";
  s += "frame: max fps " + Flag("cod3_max_fps") + ", shader threads " +
       Flag("vulkan_pipeline_creation_threads") + ", async shaders " +
       Flag("async_shader_compilation") + "\n";
  s += "input: mnk " + Flag("mnk_mode") + ", raw mouse " + Flag("cod3_raw_mouse") +
       ", sensitivity " + Flag("cod3_sensitivity") + "\n";
  s += "game: skip cutscenes " + Flag("cod3_skip_movies") + ", volume " + Flag("cod3_volume") +
       ", log detail " + Flag("cod3_log_detail") + "\n";
  return s;
}

void Start(const std::filesystem::path& logs_dir) {
  g_logs_dir = logs_dir;
  ApplyDetail(REXCVAR_GET(cod3_log_detail));
  rex::cvar::RegisterChangeCallback("cod3_log_detail", [](std::string_view, std::string_view value) {
    ApplyDetail(std::string(value));
    REXLOG_INFO("cod3: log detail {}", value);
  });
  // Settings changed while playing (F2, console): one line each when verbose.
  for (const char* name :
       {"fullscreen", "cod3_render_resolution", "cod3_refresh_rate", "cod3_graphics_api",
        "native_2x_msaa", "swap_post_effect", "readback_resolve", "render_target_path_vulkan",
        "render_target_path_d3d12", "cod3_max_fps", "resolution_scale", "cod3_skip_movies",
        "cod3_hd_textures"}) {
    if (!rex::cvar::GetFlagInfo(name)) continue;
    rex::cvar::RegisterChangeCallback(name, [](std::string_view n, std::string_view value) {
      if (Verbose()) REXLOG_INFO("cod3: setting {} = {}", n, value);
    });
  }
  REXLOG_INFO("cod3: ---- session ---- {}; game built {} {}; {}; {} CPU threads",
              REXGLUE_BUILD_STAMP, __DATE__, __TIME__, OsSummary(),
              std::max(1u, std::thread::hardware_concurrency()));
  std::string summary = SettingsSummary();
  size_t start = 0;
  while (start < summary.size()) {
    const size_t end = summary.find('\n', start);
    REXLOG_INFO("cod3:   {}", summary.substr(start, end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  REXLOG_INFO("cod3:   log detail {} (cod3_log_detail; `cod3_report` writes a report)",
              REXCVAR_GET(cod3_log_detail));
}

void OnFrame(double frame_s, double busy_s) {
#if COD3_HAS_GPU_STATS
  namespace gs = rex::graphics::stats;
  const gs::Snapshot cur = gs::Read();
  const gs::Snapshot prev = g_last_gpu;
  g_last_gpu = cur;
#endif
  if (frame_s <= 0.0 || frame_s > 3.0) return;  // a load or a stall, not a frame
  const double avg = g_avg_frame_s > 0.0 ? g_avg_frame_s : frame_s;
  g_avg_frame_s = avg * 0.95 + frame_s * 0.05;
  if (!Verbose()) return;
  // Slow: over 2.5x the running average and over 25 ms.
  if (frame_s < 0.025 || frame_s < avg * 2.5) return;
  char line[512];
#if COD3_HAS_GPU_STATS
  auto d = [&](gs::Counter c) { return (unsigned long long)(cur[c] - prev[c]); };
  std::snprintf(line, sizeof(line),
                "slow frame %.1f ms (average %.1f ms, game thread %.1f ms) | GPU since the last "
                "frame: %llu draws, %llu resolves, %llu transfers, %llu waits (%.1f ms), %llu "
                "readbacks (%llu KB), upload %llu KB, %llu texture loads, %llu watch faults",
                frame_s * 1000.0, avg * 1000.0, busy_s * 1000.0, d(gs::Counter::kDraws),
                d(gs::Counter::kResolves), d(gs::Counter::kRenderTargetTransfers),
                d(gs::Counter::kGpuWaits), double(d(gs::Counter::kGpuWaitMicros)) / 1000.0,
                d(gs::Counter::kReadbacks), d(gs::Counter::kReadbackBytes) / 1024,
                d(gs::Counter::kUploadBytes) / 1024, d(gs::Counter::kTextureLoads),
                d(gs::Counter::kWatchFaults));
#else
  std::snprintf(line, sizeof(line), "slow frame %.1f ms (average %.1f ms, game thread %.1f ms)",
                frame_s * 1000.0, avg * 1000.0, busy_s * 1000.0);
#endif
  {
    std::lock_guard lock(g_mutex);
    g_slow_frames.push_back({Timestamp(), line, frame_s * 1000.0});
    if (g_slow_frames.size() > kSlowFramesKept) g_slow_frames.pop_front();
  }
  // At most a few lines a second in the log; the rest are counted.
  const auto now = Clock::now();
  if (now - g_last_slow_log < std::chrono::milliseconds(250)) {
    ++g_slow_suppressed;
    return;
  }
  g_last_slow_log = now;
  if (g_slow_suppressed) {
    REXLOG_INFO("cod3: {} ({} more slow frames since the last line)", line, g_slow_suppressed);
    g_slow_suppressed = 0;
  } else {
    REXLOG_INFO("cod3: {}", line);
  }
}

void LevelLoadBegin(const std::string& map) {
  g_load_map = map;
  g_load_start = Clock::now();
  g_load_movie = {};
  REXLOG_INFO("cod3: loading level {}", map);
}

void LevelLoadMovie() {
  if (g_load_start != Clock::time_point{} && g_load_movie == Clock::time_point{})
    g_load_movie = Clock::now();
}

void LevelLoadEnd() {
  if (g_load_start == Clock::time_point{}) return;
  const auto end = Clock::now();
  auto secs = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
  char line[256];
  if (g_load_movie != Clock::time_point{})
    std::snprintf(line, sizeof(line), "%s: loaded in %.1f s, then the briefing played %.1f s more",
                  g_load_map.c_str(), secs(g_load_movie - g_load_start), secs(end - g_load_movie));
  else
    std::snprintf(line, sizeof(line), "%s: loaded in %.1f s", g_load_map.c_str(),
                  secs(end - g_load_start));
  REXLOG_INFO("cod3: level {}", line);
  {
    std::lock_guard lock(g_mutex);
    g_level_loads.push_back(Timestamp() + "  " + line);
  }
  g_load_start = {};
  g_avg_frame_s = 0.0;  // a new level: don't compare with the menu's frame times
}

void RecordPerfLine(const std::string& line) {
  std::lock_guard lock(g_mutex);
  g_perf_lines.push_back(Timestamp() + "  " + line);
  if (g_perf_lines.size() > kPerfLinesKept) g_perf_lines.pop_front();
}

// [hw-scaling]
std::filesystem::path LogsDir() { return g_logs_dir; }

std::filesystem::path WriteReport() {
  std::error_code ec;
  std::filesystem::create_directories(g_logs_dir, ec);
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char name[64];
  std::strftime(name, sizeof(name), "report-%Y%m%d-%H%M%S.txt", &tm);
  const auto path = g_logs_dir / name;
  std::ofstream out(path, std::ios::trunc);
  if (!out) return {};
  out << "Call of Duty 3 (recomp) report\n";
  out << REXGLUE_BUILD_STAMP << "; game built " << __DATE__ << " " << __TIME__ << "\n";
  out << OsSummary() << "; " << std::max(1u, std::thread::hardware_concurrency())
      << " CPU threads\n\n";
  out << "== Settings that matter ==\n" << SettingsSummary() << "\n";
  {
    // [hw-scaling] what this PC has, and where the time goes per thread.
    const auto& machine = machine::Get();
    out << "== This PC ==\n" << machine.Describe();
    out << "Thread plan: " << machine::Budget(machine).Describe() << "\n\n";
    const ThreadReport threads = ThreadProfiler::Get().Report();
    out << "== Threads, last second (CPU % of one core; waits % of the time) ==\n";
    const std::string verdict = ThreadProfiler::Verdict(threads);
    if (!verdict.empty()) out << verdict << "\n";
    out << ThreadProfiler::Table(threads, 24) << "\n";
  }
  out << "== Settings changed from their defaults ==\n";
  for (const auto& n : rex::cvar::ListModifiedFlags()) {
    // Paths can contain the user's name; leave them out.
    if (n.find("path") != std::string::npos || n.find("root") != std::string::npos ||
        n.find("file") != std::string::npos)
      continue;
    out << n << " = " << rex::cvar::GetFlagByName(n) << "\n";
  }
  std::lock_guard lock(g_mutex);
  out << "\n== Level loads ==\n";
  for (const auto& l : g_level_loads) out << l << "\n";
  out << "\n== Cost per frame, once a second (newest last) ==\n";
  for (const auto& l : g_perf_lines) out << l << "\n";
  out << "\n== Slow frames (newest last; logged with cod3_log_detail verbose) ==\n";
  for (const auto& f : g_slow_frames) out << f.when << "  " << f.line << "\n";
  std::vector<SlowFrame> worst(g_slow_frames.begin(), g_slow_frames.end());
  std::sort(worst.begin(), worst.end(), [](const SlowFrame& a, const SlowFrame& b) { return a.ms > b.ms; });
  out << "\n== The 5 slowest of those ==\n";
  for (size_t i = 0; i < worst.size() && i < 5; ++i) out << worst[i].when << "  " << worst[i].line << "\n";
  out.close();
  REXLOG_INFO("cod3: wrote the report {}", PathToUtf8(path));
  return path;
}

}  // namespace cod3::diag
