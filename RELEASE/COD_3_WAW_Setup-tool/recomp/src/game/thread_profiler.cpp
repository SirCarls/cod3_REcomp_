// cod3 - the in-engine thread profiler (see thread_profiler.h).

#include "game/thread_profiler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

#if __has_include(<rex/graphics/gpu_stats.h>)
#include <rex/graphics/gpu_stats.h>
#define COD3_HAS_GPU_STATS 1
#else
#define COD3_HAS_GPU_STATS 0
#endif

#if __has_include(<rex/thread/thread_stats.h>)
#include <rex/thread/thread_stats.h>
#define COD3_HAS_THREAD_STATS 1
#else
#define COD3_HAS_THREAD_STATS 0
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#endif

#include "game/diagnostics.h"
#include "game/frame_rate.h"
#include "game/function_profiler.h"
#include "game/machine_profile.h"
#include "game/program.h"
#include "game/watchdog.h"

REXCVAR_DECLARE(int32_t, cod3_perf_log);          // game/perf_stats.cpp
REXCVAR_DECLARE(int32_t, cod3_watchdog_seconds);  // game/watchdog.cpp

REXCVAR_DEFINE_BOOL(cod3_profile_auto, true, "cod3/Performance",
                    "Once a session, after a minute of play, find out which functions the busy "
                    "frame threads (GPU emulation, the game's main thread, the window) spend "
                    "their time in, 10 s each; the reports go to userdata/logs and into "
                    "cod3_report (cod3_profile in the F10 console does it by hand)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3 {

namespace {

uint64_t NowMicros() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

struct OsThread {
  uint32_t id = 0;
  uint64_t cpu_ns = 0;
  uint64_t created = 0;  // Windows: creation time; the earliest is the window thread
  std::string name;
};

#if defined(_WIN32)

std::string Narrow(const wchar_t* text) {
  if (!text || !*text) return {};
  char out[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out) - 1, nullptr, nullptr);
  return out;
}

std::vector<OsThread> ReadOsThreads() {
  std::vector<OsThread> threads;
  // GetThreadDescription: Windows 10 1607 and later (and recent Wine).
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  const DWORD pid = GetCurrentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return threads;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
    if (!thread) continue;
    OsThread t;
    t.id = entry.th32ThreadID;
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
      auto ticks = [](const FILETIME& f) {
        return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
      };
      t.cpu_ns = (ticks(kernel) + ticks(user)) * 100;  // 100 ns units
      t.created = ticks(created);
    }
    if (get_description) {
      PWSTR description = nullptr;
      if (SUCCEEDED(get_description(thread, &description)) && description) {
        t.name = Narrow(description);
        LocalFree(description);
      }
    }
    CloseHandle(thread);
    threads.push_back(std::move(t));
  }
  CloseHandle(snapshot);
  return threads;
}

#else

std::string ReadSmallFile(const std::string& path) {
  std::ifstream in(path);
  std::string text;
  std::getline(in, text);
  return text;
}

std::vector<OsThread> ReadOsThreads() {
  std::vector<OsThread> threads;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return threads;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    OsThread t;
    t.id = uint32_t(std::strtoul(e->d_name, nullptr, 10));
    const std::string base = std::string("/proc/self/task/") + e->d_name;
    // schedstat: nanoseconds on a CPU, then wait time, then run count.
    const std::string sched = ReadSmallFile(base + "/schedstat");
    if (!sched.empty()) {
      t.cpu_ns = std::strtoull(sched.c_str(), nullptr, 10);
    } else {
      // Older kernels without schedstat: utime + stime in clock ticks.
      const std::string stat = ReadSmallFile(base + "/stat");
      const size_t close = stat.rfind(')');
      if (close != std::string::npos) {
        std::istringstream rest(stat.substr(close + 2));
        std::string field;
        uint64_t utime = 0, stime = 0;
        for (int i = 3; rest >> field; ++i) {
          if (i == 14) utime = std::strtoull(field.c_str(), nullptr, 10);
          if (i == 15) {
            stime = std::strtoull(field.c_str(), nullptr, 10);
            break;
          }
        }
        const long hz = sysconf(_SC_CLK_TCK);
        if (hz > 0) t.cpu_ns = (utime + stime) * (1000000000ull / uint64_t(hz));
      }
    }
    t.name = ReadSmallFile(base + "/comm");
    threads.push_back(std::move(t));
  }
  closedir(dir);
  return threads;
}

#endif

// What a thread is, from its name or what we know about it.
bool StartsWith(const std::string& text, const char* prefix) {
  return text.rfind(prefix, 0) == 0;
}

// What a thread is, from its name or what we know about it. The SDK names its
// threads "<name> (<handle>)" and Linux keeps only 15 characters
// ("GPU Commands (F"), so names are matched by their start.
// The process's own name. On Linux a thread nobody named carries it (the
// graphics driver's queue threads, for one: lavapipe's in the cloud tests,
// NVIDIA's on many PCs); on Windows such a thread has no name at all.
const std::string& ProcessName() {
  static const std::string name = [] {
#if defined(__linux__)
    std::ifstream in("/proc/self/comm");
    std::string line;
    std::getline(in, line);
    return line;
#else
    return std::string();
#endif
  }();
  return name;
}

std::string RoleOf(const std::string& name, uint32_t id, uint32_t main_id, uint32_t ui_id) {
  if (id == main_id || StartsWith(name, "Main XThread")) return "game main thread";
  if (id == ui_id) return "window, input, presenting";
  if (name.empty() || name == ProcessName()) return "unnamed: a driver or library";
  if (StartsWith(name, "GPU Commands")) return "GPU emulation";
  if (StartsWith(name, "GPU VSync")) return "guest vblank timer";
  if (StartsWith(name, "Vulkan Pipeline") || StartsWith(name, "D3D12 Pipeline"))
    return "pipeline builder";
  if (StartsWith(name, "Shader Transl")) return "shader translation";
  if (StartsWith(name, "Audio") || StartsWith(name, "XMA")) return "audio";
  if (StartsWith(name, "llvmpipe") || StartsWith(name, "WSI ")) return "graphics driver";
  if (StartsWith(name, "cod3 profiler")) return "this profiler";
  // The game's own threads, by the guest function they start in (the SDK
  // names unnamed guest threads "XThread<start address>").
  if (StartsWith(name, COD3_TEXT("XThread8212A780", "XThread821424B0")))
    return "game job worker";  // loop 0x8212A520: render job, decompression
  if (StartsWith(name, COD3_TEXT("XThread82302C80", "XThread822D1870")))
    return "game's Direct3D thread";  // waits, then D3D 0x822F4AA0
  if (StartsWith(name, "XThread")) return "game thread";
  if (StartsWith(name, "rex::thread::Ti")) return "SDK timers";
  return "";
}

}  // namespace

ThreadProfiler& ThreadProfiler::Get() {
  // Never destroyed: its thread runs until the process ends.
  static ThreadProfiler* instance = [] {
    auto* profiler = new ThreadProfiler();
    profiler->Start();
    return profiler;
  }();
  return *instance;
}

namespace {
// Started when the game loads; the thread waits until the game runs.
[[maybe_unused]] const bool g_profiler_started = (ThreadProfiler::Get(), true);
}  // namespace

void ThreadProfiler::NoteGameMainThread() {
  main_thread_id_.store(rex::thread::current_thread_system_id(), std::memory_order_relaxed);
}

void ThreadProfiler::Start() {
  std::lock_guard lock(mutex_);
  if (worker_started_) return;
  worker_started_ = true;
  std::thread([this] { WorkerLoop(); }).detach();
}

void ThreadProfiler::WorkerLoop() {
#if defined(_WIN32)
  using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  if (auto set_description = reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void*>(
          GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
    set_description(GetCurrentThread(), L"cod3 profiler");
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), "cod3 profiler");
#endif
  using Clock = std::chrono::steady_clock;
  const auto kPeriod = std::chrono::seconds(1);
  auto next = Clock::now() + kPeriod;
  bool announced = false;
  Clock::time_point last_verdict_log{}, last_table_log{}, last_hitch_log{};
  // game/frame_rate.h publishes a new period when frames arrive; a slow
  // frame counts once, in the period that holds it.
  FrameStats last_game{};
  // A stall: the game thread passes none of the watchdog's marks (frames,
  // loader steps, script threads: game/watchdog.h) for cod3_watchdog_seconds.
  // FrameRate leaves frames over 3 s out of its stats, so a stall would
  // otherwise never reach the hitch tables below. The table then says what
  // every other thread does meanwhile: the GPU thread busy or idle, the job
  // workers working or waiting, a lock held.
  uint64_t seen_marks = 0;
  Clock::time_point marks_since{};
  Clock::time_point last_stall_table{};
  int stall_tables = 0;
  // cod3_profile_auto: seconds of play so far, and the threads still to
  // profile once that reaches a minute.
  int play_seconds = 0;
  bool auto_planned = false;
  std::vector<std::string> auto_queue;
  int auto_window_tries = 0;
#if COD3_HAS_GPU_STATS
  namespace gs = rex::graphics::stats;
  gs::Snapshot last{};
  bool have_last = false;
#endif
  for (;;) {
    std::this_thread::sleep_until(next);
    const auto now = Clock::now();
    next = std::max(next + kPeriod, now + std::chrono::milliseconds(200));

    // Nothing to measure until the game runs its frames.
    const FrameStats game = FrameRate::Get().Stats();
    if (!game.valid) continue;
    const bool new_period = game.max_ms != last_game.max_ms || game.fps != last_game.fps ||
                            game.busy_ms != last_game.busy_ms;
    last_game = game;
    if (!announced) {
      announced = true;
      const auto& machine = machine::Get();
      REXLOG_INFO("cod3: machine: {}", machine.Summary());
      REXLOG_INFO("cod3: thread plan for it: {}", machine::Budget(machine).Describe());
    }

    FrameCosts costs;
    costs.game_ms = game.busy_ms;
    costs.game_fps = game.fps;
    costs.max_fps = game.limit;
    costs.slowest_frame_ms = new_period ? game.max_ms : 0.0f;
    costs.frame_ms = game.frame_ms;  // replaced by presented frames below when known
    costs.valid = true;
#if COD3_HAS_GPU_STATS
    {
      const gs::Snapshot cur = gs::Read();
      if (have_last) {
        auto delta = [&](gs::Counter c) { return double(cur[c] - last[c]); };
        const double frames = delta(gs::Counter::kFrames);
        if (frames >= 1) {
          const double secs = double(cur.time_us - last.time_us) / 1e6;
          auto per_frame_ms = [&](gs::Counter c) { return float(delta(c) / frames / 1000.0); };
          costs.frame_ms = float(secs * 1000.0 / frames);
          costs.gpu_wait_ms = per_frame_ms(gs::Counter::kGpuWaitMicros);
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 2
          costs.gpu_thread_ms = per_frame_ms(gs::Counter::kGpuThreadMicros);
          costs.submit_ms = per_frame_ms(gs::Counter::kSubmitMicros);
          costs.shader_ms = per_frame_ms(gs::Counter::kShaderTranslateMicros);
#endif
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 3
          costs.presents_per_s = float(delta(gs::Counter::kPresents) / secs);
#endif
        }
      }
      last = cur;
      have_last = true;
    }
#endif
    Compute(costs);

    // The log (see the header for what each detail level writes).
    const ThreadReport r = Report();
    if (!r.valid) continue;
    const auto detail = diag::GetDetail();
    const bool verbose = detail >= diag::Detail::kVerbose;
    auto log_table = [&](const std::string& title) {
      REXLOG_INFO("cod3: profiler: {}", title);
      const std::string table = Table(r, 12);
      size_t start = 0;
      while (start < table.size()) {
        const size_t end = table.find('\n', start);
        REXLOG_INFO("cod3:   {}", table.substr(start, end - start));
        if (end == std::string::npos) break;
        start = end + 1;
      }
    };

    // Stalls first, at the watchdog's limit, so its lines (where the game
    // thread is) and this table (what the others do) sit together: once when
    // the stall starts, then every minute, three times at most. While stalled,
    // the frame costs are stale, so no verdict and no hitch table.
    const uint64_t marks = watchdog::MarkCount();
    if (marks != seen_marks) {
      seen_marks = marks;
      marks_since = now;
      stall_tables = 0;
    } else if (marks != 0) {
      const int limit = REXCVAR_GET(cod3_watchdog_seconds);
      const double stalled_s = std::chrono::duration<double>(now - marks_since).count();
      if (limit > 0 && stalled_s >= limit) {
        if (stall_tables < 3 &&
            (stall_tables == 0 || now - last_stall_table >= std::chrono::seconds(60))) {
          ++stall_tables;
          last_stall_table = now;
          char title[160];
          std::snprintf(title, sizeof(title),
                        "the game's main thread has made no progress for %.0f s; every thread "
                        "over the last second:",
                        stalled_s);
          log_table(title);
        }
        continue;
      }
    }

    const std::string verdict = Verdict(r);
    int every = REXCVAR_GET(cod3_perf_log);
    if (every <= 0) every = detail == diag::Detail::kDebug ? 2 : verbose ? 10 : 0;
    if (every > 0 && !verdict.empty() && now - last_verdict_log >= std::chrono::seconds(every)) {
      last_verdict_log = now;
      REXLOG_INFO("cod3: profiler: {}; busiest: {}", verdict, Busiest(r, 3));
      if (const std::string job = RenderJobLine(r); !job.empty())
        REXLOG_INFO("cod3: profiler: {}", job);
    }
    const float hitch_ms = verbose ? 100.0f : 250.0f;
    const auto hitch_gap = verbose ? std::chrono::seconds(5) : std::chrono::seconds(10);
    if (costs.slowest_frame_ms >= hitch_ms && now - last_hitch_log >= hitch_gap) {
      last_hitch_log = now;
      char title[160];
      std::snprintf(title, sizeof(title), "a %.0f ms frame; %s",
                    costs.slowest_frame_ms, verdict.empty() ? "threads:" : verdict.c_str());
      log_table(title);
    } else if (verbose && now - last_table_log >= std::chrono::seconds(60)) {
      last_table_log = now;
      log_table("threads over the last second; " + verdict);
    }

    // cod3_profile_auto: after a minute of play (the game making frames, not
    // stalled), plan which frame threads to profile, then profile them one by
    // one.
    if (costs.game_fps >= 5.0f) ++play_seconds;
    if (!auto_planned && play_seconds >= 60 && REXCVAR_GET(cod3_profile_auto)) {
      auto_planned = true;
      auto row_cpu = [&](const char* role) {
        for (const ThreadRow& row : r.rows)
          if (row.role == role) return row.cpu_pct;
        return 0.0f;
      };
      // The limiting thread first.
      const bool main_limits = r.limited_by.find("main thread") != std::string::npos;
      if (main_limits && row_cpu("game main thread") >= 50.0f) auto_queue.push_back("main");
      if (row_cpu("GPU emulation") >= 40.0f) auto_queue.push_back("gpu");
      if (!main_limits && row_cpu("game main thread") >= 50.0f) auto_queue.push_back("main");
      if (row_cpu("window, input, presenting") >= 40.0f) auto_queue.push_back("window");
      // The thread holding the SDK's global lock while the GPU emulation waits
      // for it: its profile says what it does with the lock held.
      for (const ThreadRow& row : r.rows) {
        if (row.role != "GPU emulation" || row.lock_pct < 5.0f || row.top_lock_holder == 0) continue;
        std::string holder = "#" + std::to_string(row.top_lock_holder);
        for (const ThreadRow& other : r.rows)
          if (other.id == row.top_lock_holder && other.role == "game main thread") holder = "main";
        if (std::find(auto_queue.begin(), auto_queue.end(), holder) == auto_queue.end())
          auto_queue.push_back(holder);
      }
      std::string list;
      for (const auto& t : auto_queue) list += (list.empty() ? "" : ", ") + t;
      REXLOG_INFO("cod3: profiler: after a minute of play, profiling {} for 10 s each "
                  "(cod3_profile_auto; reports in userdata/logs and in cod3_report)",
                  list.empty() ? std::string("no thread (none is busy)") : list);
    }
    if (!auto_queue.empty() && !FunctionProfiler::Busy() && REXCVAR_GET(cod3_profile_auto)) {
      const std::string next_thread = auto_queue.front();
      // The window thread only while it isn't repainting on its own (an open
      // console or F2 would be what's measured); give that a minute.
      const float frames_per_s = costs.frame_ms > 0 ? 1000.0f / costs.frame_ms : 0.0f;
      const bool repainting = costs.presents_per_s > frames_per_s * 1.3f + 5.0f;
      if (next_thread == "window" && repainting && ++auto_window_tries < 60) {
        // try again next second
      } else {
        auto_queue.erase(auto_queue.begin());
        if (!(next_thread == "window" && repainting)) {
          const std::string message = FunctionProfiler::Start(next_thread, 10, 0);
          if (message.rfind("Profiling", 0) != 0) REXLOG_INFO("cod3: profiler: {}", message);
        }
      }
    }
  }
}

namespace {
std::atomic<GuestGpuWaitReader> g_guest_gpu_wait_reader{nullptr};
std::atomic<RenderJobReader> g_render_job_reader{nullptr};
}  // namespace

void SetGuestGpuWaitReader(GuestGpuWaitReader reader) {
  g_guest_gpu_wait_reader.store(reader, std::memory_order_release);
}

void SetRenderJobReader(RenderJobReader reader) {
  g_render_job_reader.store(reader, std::memory_order_release);
}

void ThreadProfiler::Compute(const FrameCosts& costs) {
  const uint64_t now_us = NowMicros();
  const std::vector<OsThread> threads = ReadOsThreads();

  // Waits per thread, by OS id.
  std::unordered_map<uint32_t, std::array<uint64_t, 4>> waits;
  float lock_waits_per_s = 0, lock_spin_won_pct = -1;
  // Global lock waits per thread, by the thread that held the lock.
  std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint64_t>> holder_waits;
#if COD3_HAS_THREAD_STATS
  {
    namespace ts = rex::thread_stats;
    static std::vector<ts::ThreadTotals> totals(ts::kMaxThreads);
    const size_t n = ts::Read(totals.data(), totals.size());
    for (size_t i = 0; i < n; ++i) {
      auto& w = waits[totals[i].system_id];
      for (size_t k = 0; k < 4 && k < size_t(ts::Wait::kCount); ++k) w[k] += totals[i].wait_us[k];
    }
#if defined(REX_THREAD_STATS_VERSION) && REX_THREAD_STATS_VERSION >= 3
    {
      const ts::LockSpinTotals spins = ts::ReadLockSpins();
      if (previous_time_us_ != 0 && now_us > previous_time_us_) {
        const double won = double(spins.won - std::min(spins.won, previous_lock_spins_won_));
        const double slept = double(spins.slept - std::min(spins.slept, previous_lock_spins_slept_));
        lock_waits_per_s = float((won + slept) / (double(now_us - previous_time_us_) / 1e6));
        lock_spin_won_pct = won + slept > 0 ? float(100.0 * won / (won + slept)) : 0.0f;
      }
      previous_lock_spins_won_ = spins.won;
      previous_lock_spins_slept_ = spins.slept;
    }
#endif
#if defined(REX_THREAD_STATS_VERSION) && REX_THREAD_STATS_VERSION >= 2
    static std::vector<ts::LockHolderTotals> pairs(ts::kMaxThreads * ts::kMaxHoldersPerThread);
    const size_t p = ts::ReadLockHolders(pairs.data(), pairs.size());
    for (size_t i = 0; i < p; ++i)
      holder_waits[pairs[i].waiter_id][pairs[i].holder_id] += pairs[i].wait_us;
#endif
  }
#endif

  const uint32_t main_thread_id = main_thread_id_.load(std::memory_order_relaxed);
  if (!ui_thread_id_) {
    // The window thread is the process's first one.
#if defined(_WIN32)
    uint64_t earliest = UINT64_MAX;
    for (const OsThread& t : threads)
      if (t.created && t.created < earliest) earliest = t.created, ui_thread_id_ = t.id;
#else
    ui_thread_id_ = uint32_t(getpid());
#endif
  }
  const uint32_t ui_thread_id = ui_thread_id_;
  const int usable_cpus = machine::Get().UsableCpus();
  const uint64_t period_us = previous_time_us_ ? now_us - previous_time_us_ : 0;
  previous_time_us_ = now_us;

  ThreadReport r;
  r.costs = costs;
  r.lock_waits_per_s = lock_waits_per_s;
  r.lock_spin_won_pct = lock_spin_won_pct;
  if (const GuestGpuWaitReader reader = g_guest_gpu_wait_reader.load(std::memory_order_acquire)) {
    constexpr size_t kGroups = size_t(GuestGpuWait::kCount);
    constexpr size_t kAll = kGroups + kGuestGpuWaitRawReasons;
    uint64_t now_waits[kAll] = {};
    reader(now_waits, kAll);
    if (period_us > 0 && have_guest_gpu_waits_) {
      auto ms_a_second = [&](size_t i) {
        return float(double(now_waits[i] - std::min(now_waits[i], previous_guest_gpu_waits_[i])) /
                     1000.0 / (double(period_us) / 1e6));
      };
      for (size_t i = 0; i < kGroups; ++i) r.guest_gpu_wait_ms[i] = ms_a_second(i);
      // Which code the "other" waits mostly were (docs/COD3_D3D.md names the
      // known ones), for the next look at Direct3D.
      static constexpr uint32_t kKnown[] = {1, 2, 6, 0xA, 0xC, 0xE};
      for (uint32_t code = 0; code < kGuestGpuWaitRawReasons; ++code) {
        const bool known = std::find(std::begin(kKnown), std::end(kKnown), code) != std::end(kKnown) ||
                           (code >= 7 && code <= 0x11);
        const float ms = ms_a_second(kGroups + code);
        if (!known && ms > r.guest_gpu_other_reason_ms) {
          r.guest_gpu_other_reason = int(code);
          r.guest_gpu_other_reason_ms = ms;
        }
      }
    }
    for (size_t i = 0; i < kAll; ++i) previous_guest_gpu_waits_[i] = now_waits[i];
    have_guest_gpu_waits_ = true;
  }
  if (const RenderJobReader reader = g_render_job_reader.load(std::memory_order_acquire)) {
    RenderJobTotals now_job;
    reader(&now_job);
    const RenderJobTotals& was = previous_render_job_;
    if (period_us > 0 && have_render_job_ && now_job.jobs >= was.jobs) {
      const double secs = double(period_us) / 1e6;
      const uint64_t jobs = now_job.jobs - was.jobs;
      r.render_jobs_per_s = float(double(jobs) / secs);
      r.swaps_elsewhere_per_s = float(double(now_job.swaps_elsewhere - was.swaps_elsewhere) / secs);
      if (jobs > 0) {
        auto per_job_ms = [&](uint64_t now_us, uint64_t was_us) {
          return float(double(now_us - std::min(now_us, was_us)) / 1000.0 / double(jobs));
        };
        r.render_job_ms = per_job_ms(now_job.job_wall_us, was.job_wall_us);
        r.render_job_cpu_ms = per_job_ms(now_job.job_cpu_us, was.job_cpu_us);
        r.back_end_ms = per_job_ms(now_job.back_end_wall_us, was.back_end_wall_us);
        r.back_end_cpu_ms = per_job_ms(now_job.back_end_cpu_us, was.back_end_cpu_us);
      }
    }
    previous_render_job_ = now_job;
    have_render_job_ = true;
  }
  std::unordered_map<uint32_t, Previous> next;
  for (const OsThread& t : threads) {
    Previous cur;
    cur.cpu_ns = t.cpu_ns;
    if (auto it = waits.find(t.id); it != waits.end())
      for (int k = 0; k < 4; ++k) cur.wait_us[k] = it->second[k];
    if (auto it = holder_waits.find(t.id); it != holder_waits.end()) cur.holder_us = it->second;
    const auto prev = previous_.find(t.id);
    if (period_us > 0 && prev != previous_.end()) {
      ThreadRow row;
      row.id = t.id;
      row.name = t.name.empty() ? "thread " + std::to_string(t.id) : t.name;
      // The SDK names threads "<name> (<handle>)"; Linux keeps 15 characters,
      // which leaves a stub like "Main XThread (F". Show the name only.
      if (const size_t paren = row.name.find(" ("); paren != std::string::npos && paren > 0)
        row.name.resize(paren);
      row.role = RoleOf(t.name, t.id, main_thread_id, ui_thread_id);
      const double period = double(period_us);
      row.cpu_pct = float(double(cur.cpu_ns - std::min(cur.cpu_ns, prev->second.cpu_ns)) / 1000.0 /
                          period * 100.0);
      auto wait_pct = [&](int k) {
        const uint64_t d = cur.wait_us[k] - std::min(cur.wait_us[k], prev->second.wait_us[k]);
        return float(std::min(100.0, double(d) / period * 100.0));
      };
      row.guest_wait_pct = wait_pct(0);
      row.sleep_pct = wait_pct(1);
      row.spin_pct = wait_pct(2);
      row.lock_pct = wait_pct(3);
      // A wait is counted when it ends, so one that began in an earlier
      // second can add more than this second holds. Running (minus spinning,
      // which is CPU time too) plus waiting can't exceed the second: scale the
      // waits down to fit.
      const float waiting = row.guest_wait_pct + row.sleep_pct + row.lock_pct;
      const float room = std::max(0.0f, 100.0f - std::max(0.0f, row.cpu_pct - row.spin_pct));
      float lock_scale = 1.0f;
      if (waiting > room && waiting > 0) {
        const float scale = room / waiting;
        row.guest_wait_pct *= scale;
        row.sleep_pct *= scale;
        row.lock_pct *= scale;
        lock_scale = scale;
      }
      // Who held the lock: names are filled in below, once every row is known.
      if (row.lock_pct >= 1.0f) {
        std::vector<std::pair<float, uint32_t>> holders;
        for (const auto& [holder, us] : cur.holder_us) {
          uint64_t before = 0;
          if (auto h = prev->second.holder_us.find(holder); h != prev->second.holder_us.end())
            before = h->second;
          const float pct =
              float(std::min(100.0, double(us - std::min(us, before)) / period * 100.0)) *
              lock_scale;
          if (pct >= 0.5f) holders.push_back({pct, holder});
        }
        std::sort(holders.begin(), holders.end(), std::greater<>());
        if (holders.size() > 3) holders.resize(3);
        if (!holders.empty()) row.top_lock_holder = holders[0].second;
        for (const auto& [pct, holder] : holders) {
          char part[32];
          std::snprintf(part, sizeof(part), "%s#%u %.0f%%", row.lock_holders.empty() ? "" : ", ",
                        holder, pct);
          row.lock_holders += part;
        }
      }
      r.process_cpu_pct += row.cpu_pct;
      if (row.cpu_pct >= 85.0f) ++r.busy_threads;
      r.rows.push_back(std::move(row));
    }
    next[t.id] = cur;
  }
  previous_ = std::move(next);
  // Lock holders by name: "#<id>" becomes the holder's name (its role when
  // the name says little), "#0" "not known".
  for (ThreadRow& row : r.rows) {
    if (row.lock_holders.empty()) continue;
    std::string named;
    size_t pos = 0;
    while (pos < row.lock_holders.size()) {
      const size_t hash = row.lock_holders.find('#', pos);
      if (hash == std::string::npos) {
        named += row.lock_holders.substr(pos);
        break;
      }
      named += row.lock_holders.substr(pos, hash - pos);
      size_t end = hash + 1;
      while (end < row.lock_holders.size() && std::isdigit(static_cast<unsigned char>(row.lock_holders[end]))) ++end;
      const uint32_t id = uint32_t(std::strtoul(row.lock_holders.c_str() + hash + 1, nullptr, 10));
      std::string who = id == 0 ? "not known" : "thread " + std::to_string(id);
      for (const ThreadRow& other : r.rows)
        if (other.id == id) who = other.role.empty() || other.role.rfind("unnamed", 0) == 0
                                      ? other.name
                                      : other.name + " (" + other.role + ")";
      named += who;
      pos = end;
    }
    row.lock_holders = named;
  }
  std::sort(r.rows.begin(), r.rows.end(),
            [](const ThreadRow& a, const ThreadRow& b) { return a.cpu_pct > b.cpu_pct; });
  r.valid = period_us > 0;

  // What limits the frame rate. Checked in this order: the first that fits
  // explains the frame time. Times are per frame; a long wait (seconds on a
  // software GPU) is counted when it ends, so a period can show more than
  // its own length: shown values are capped at the frame time.
  const float F = costs.frame_ms;
  auto cap = [F](float ms) { return std::min(ms, F); };
  const ThreadRow* main = nullptr;
  const ThreadRow* gpu = nullptr;
  for (const auto& row : r.rows) {
    if (row.role == "game main thread" && !main) main = &row;
    if (row.role == "GPU emulation" && !gpu) gpu = &row;
  }
  // The main thread's CPU time per game frame (the renderer plan's ceiling,
  // next to the render job's).
  if (main && costs.valid && costs.game_fps > 0)
    r.main_cpu_ms = main->cpu_pct / 100.0f * 1000.0f / costs.game_fps;
  char why[256] = {};
  if (r.valid && costs.valid && F > 0) {
    // The limiter's wait per game frame, on the game's own clock.
    const float limiter_ms =
        costs.game_fps > 0 ? std::max(0.0f, 1000.0f / costs.game_fps - costs.game_ms) : 0.0f;
    const float main_wait_ms =
        main ? (main->guest_wait_pct + main->sleep_pct + main->lock_pct) / 100.0f * F : 0.0f;
    const float main_spin_ms = main ? main->spin_pct / 100.0f * F : 0.0f;
    const float main_work_ms = std::max(0.0f, costs.game_ms - main_wait_ms - main_spin_ms);
    const bool all_cores_busy = r.process_cpu_pct >= 90.0f * float(usable_cpus);
    // At the cap: the game's own frame rate is (nearly) com_maxfps. Judged on
    // the game's frames, not presented ones, which are counted on another clock.
    if (costs.max_fps > 0 && costs.game_fps >= 0.9f * float(costs.max_fps)) {
      r.limited_by = "the frame limit";
      std::snprintf(why, sizeof(why),
                    "com_maxfps %d; the game runs at %.0f fps and waits %.1f ms a frame",
                    costs.max_fps, costs.game_fps, limiter_ms);
    } else if (all_cores_busy) {
      // Every core this process may use is busy: a PC with few cores, other
      // programs taking them, or (in a test) a graphics driver running on
      // the CPU. Whichever thread looks slowest is only waiting its turn.
      r.limited_by = "the CPU: all its cores are busy";
      std::snprintf(why, sizeof(why), "the game uses %.0f%% of %d usable cores", r.process_cpu_pct,
                    usable_cpus);
    } else if (costs.gpu_wait_ms >= 0.30f * F) {
      r.limited_by = "the graphics card";
      std::snprintf(why, sizeof(why), "the GPU emulation waits %.1f of %.1f ms a frame for it",
                    cap(costs.gpu_wait_ms), F);
    } else if (costs.gpu_thread_ms >= 0.85f * F) {
      // Busy by the clock, but is it working? Its CPU time says.
      if (gpu && gpu->cpu_pct < 60.0f && gpu->lock_pct >= 15.0f) {
        // Not the driver: the thread sleeps on the SDK's global lock while
        // another thread holds it (overnight bench 2026-10-18: 39% of the
        // time in one level, behind the write-watch fault handler).
        r.limited_by = "the SDK's global lock, which the GPU emulation thread waits for";
        std::snprintf(why, sizeof(why),
                      "the GPU emulation thread is busy %.1f of %.1f ms a frame, uses %.0f%% of a "
                      "core and waits %.0f%% of the time for the lock (held by %.90s)",
                      cap(costs.gpu_thread_ms), F, gpu->cpu_pct, gpu->lock_pct,
                      gpu->lock_holders.empty() ? "not known" : gpu->lock_holders.c_str());
      } else if (gpu && gpu->cpu_pct < 60.0f) {
        r.limited_by = "the graphics card";
        std::snprintf(why, sizeof(why),
                      "the GPU emulation thread is busy %.1f of %.1f ms a frame but uses only %.0f%% "
                      "of a core: it is blocked handing work to the driver; submitting %.1f ms",
                      cap(costs.gpu_thread_ms), F, gpu->cpu_pct, cap(costs.submit_ms));
      } else {
        r.limited_by = "the GPU emulation thread";
        std::snprintf(why, sizeof(why),
                      "busy %.1f of %.1f ms a frame; submitting %.1f, translating shaders %.1f",
                      cap(costs.gpu_thread_ms), F, cap(costs.submit_ms), cap(costs.shader_ms));
      }
    } else if (main && main_work_ms >= 0.85f * F) {
      r.limited_by = "the game's main thread";
      std::snprintf(why, sizeof(why), "works %.1f of %.1f ms a frame", cap(main_work_ms), F);
    } else if (main && main_wait_ms + main_spin_ms >= 0.40f * F) {
      r.limited_by = costs.gpu_thread_ms >= 0.60f * F
                         ? "the GPU emulation thread, which the game waits for"
                         : "the game's other threads: its render job and workers";
      std::snprintf(why, sizeof(why),
                    "the main thread waits %.1f of %.1f ms a frame and spins %.1f; GPU thread "
                    "busy %.1f",
                    cap(main_wait_ms), F, cap(main_spin_ms), cap(costs.gpu_thread_ms));
    } else if (!r.rows.empty() && r.rows.front().cpu_pct >= 90.0f) {
      const ThreadRow& top = r.rows.front();
      r.limited_by = "thread " + top.name + (top.role.empty() ? "" : ", " + top.role);
      std::snprintf(why, sizeof(why), "%.0f%% of a core", top.cpu_pct);
    } else {
      r.limited_by = "nothing in particular";
      std::snprintf(why, sizeof(why), "no thread or core is saturated: V-Sync, presenting or a mix");
    }
    r.why = why;
  }
  // A second without the window repainting on its own (an open console or
  // F2) is kept for cod3_report, which is usually written from the console.
  const float frames_per_s = costs.frame_ms > 0 ? 1000.0f / costs.frame_ms : 0.0f;
  const bool quiet = r.valid && costs.game_fps >= 5.0f &&
                     costs.presents_per_s <= frames_per_s * 1.3f + 5.0f;
  std::lock_guard lock(mutex_);
  if (quiet) {
    quiet_report_ = r;
    quiet_time_us_ = now_us;
  }
  report_ = std::move(r);
}

ThreadReport ThreadProfiler::Report() {
  std::lock_guard lock(mutex_);
  return report_;
}

ThreadReport ThreadProfiler::QuietReport(double* age_s) {
  std::lock_guard lock(mutex_);
  *age_s = quiet_time_us_ ? double(NowMicros() - quiet_time_us_) / 1e6 : -1.0;
  return quiet_report_;
}

std::string ThreadProfiler::Verdict(const ThreadReport& r) {
  if (!r.valid || r.limited_by.empty()) return {};
  return "limited by " + r.limited_by + " (" + r.why + ")";
}

std::string ThreadProfiler::Busiest(const ThreadReport& r, size_t count) {
  std::string text;
  char part[96];
  for (size_t i = 0; i < r.rows.size() && i < count; ++i) {
    std::snprintf(part, sizeof(part), "%s%s %.0f%%", i ? ", " : "", r.rows[i].name.c_str(),
                  r.rows[i].cpu_pct);
    text += part;
  }
  return text;
}

std::string ThreadProfiler::RenderJobLine(const ThreadReport& r) {
  if (!r.valid || r.render_jobs_per_s < 0.5f) return "";
  char line[320];
  int n = std::snprintf(line, sizeof(line),
                        "the game's render job %.2f ms a frame (%.2f ms of CPU; its back end %.2f ms, "
                        "%.2f of CPU), %.0f jobs a second",
                        r.render_job_ms, r.render_job_cpu_ms, r.back_end_ms, r.back_end_cpu_ms,
                        r.render_jobs_per_s);
  if (r.main_cpu_ms >= 0 && n > 0 && size_t(n) < sizeof(line))
    n += std::snprintf(line + n, sizeof(line) - size_t(n),
                       "; the game's main thread %.2f ms of CPU a frame", r.main_cpu_ms);
  if (r.swaps_elsewhere_per_s >= 0.5f && n > 0 && size_t(n) < sizeof(line))
    std::snprintf(line + n, sizeof(line) - size_t(n),
                  "; Direct3D swapped %.0f times a second outside the render job",
                  r.swaps_elsewhere_per_s);
  return line;
}

std::string ThreadProfiler::Table(const ThreadReport& r, size_t max_rows) {
  std::ostringstream s;
  if (!r.valid) return "(no thread sample yet)\n";
  char line[256];
  std::snprintf(line, sizeof(line), "all threads %.0f%% of one core; %d above 85%%",
                r.process_cpu_pct, r.busy_threads);
  s << line;
  if (r.lock_spin_won_pct >= 0 && r.lock_waits_per_s >= 1.0f) {
    std::snprintf(line, sizeof(line), "; global lock waits %.0f a second, %.0f%% ended while spinning",
                  r.lock_waits_per_s, r.lock_spin_won_pct);
    s << line;
  }
  s << "\n";
  // The game's waits for the emulated GPU, by what its Direct3D waited for.
  if (r.guest_gpu_wait_ms[0] >= 0) {
    static const char* const kNames[] = {"ring space", "command memory", "fences", "locks",
                                         "resource release", "other"};
    float total = 0;
    std::string parts;
    for (size_t i = 0; i < size_t(GuestGpuWait::kCount); ++i) {
      total += r.guest_gpu_wait_ms[i];
      if (r.guest_gpu_wait_ms[i] < 0.5f) continue;
      std::snprintf(line, sizeof(line), "%s%s %.0f", parts.empty() ? "" : ", ", kNames[i],
                    r.guest_gpu_wait_ms[i]);
      parts += line;
    }
    if (r.guest_gpu_other_reason >= 0 && r.guest_gpu_other_reason_ms >= 0.5f) {
      std::snprintf(line, sizeof(line), "; other mostly reason 0x%X%s, %.0f", r.guest_gpu_other_reason,
                    r.guest_gpu_other_reason == int(kGuestGpuWaitRawReasons - 1) ? " or above" : "",
                    r.guest_gpu_other_reason_ms);
      parts += line;
    }
    if (total >= 0.5f) {
      std::snprintf(line, sizeof(line), "the game's Direct3D waited for the GPU emulation %.0f ms a second, its threads together (%s)\n",
                    total, parts.c_str());
      s << line;
    }
  }
  if (const std::string job = RenderJobLine(r); !job.empty()) s << job << "\n";
  for (size_t i = 0; i < r.rows.size() && i < max_rows; ++i) {
    const ThreadRow& t = r.rows[i];
    if (t.cpu_pct < 0.5f && t.guest_wait_pct < 0.5f && t.lock_pct < 0.5f) break;
    std::snprintf(line, sizeof(line), "%5.0f%%  %-16s %-26s", t.cpu_pct, t.name.c_str(),
                  t.role.c_str());
    s << line;
    std::string waits;
    auto add = [&](const char* what, float pct) {
      if (pct < 1.0f) return;
      char part[48];
      std::snprintf(part, sizeof(part), "%s%s %.0f%%", waits.empty() ? "" : ", ", what, pct);
      waits += part;
    };
    add("waits", t.guest_wait_pct);
    add("sleeps", t.sleep_pct);
    add("spins", t.spin_pct);
    add("global lock", t.lock_pct);
    if (!waits.empty()) s << " " << waits;
    if (t.lock_pct >= 1.0f && !t.lock_holders.empty()) s << " (held by " << t.lock_holders << ")";
    s << "\n";
  }
  return s.str();
}

}  // namespace cod3
