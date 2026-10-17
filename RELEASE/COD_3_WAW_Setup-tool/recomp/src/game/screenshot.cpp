// cod3 - screenshots for debugging and bug reports (see screenshot.h).

#include "game/screenshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "util.h"

REXCVAR_DEFINE_INT32(cod3_debug_screenshots, 60, "cod3/Performance",
                     "With cod3_log_detail debug: a screenshot of the game's picture every this "
                     "many seconds while it changes, into userdata/logs/screenshots (0 = none). "
                     "Only the game's own picture is saved, never the desktop or another window.")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::shot {

namespace {

using Clock = std::chrono::steady_clock;

// The newest automatic screenshots kept in the folder (auto-*.png only).
constexpr size_t kAutoKept = 100;
// The largest picture written (the SDK's render targets are smaller).
constexpr uint32_t kMaxSide = 16384;

struct Job {
  Kind kind = Kind::kAsked;
  std::string reason;
  std::string file;       // name inside the folder
  bool periodic = false;  // the debug log's every-N-seconds one
  double seconds_off = 0;  // kCompare: how long the HD textures stay off
};

std::mutex g_mutex;  // everything below
std::condition_variable g_cv;
std::deque<Job> g_jobs;
std::filesystem::path g_folder;
CaptureFn g_capture;
std::thread g_worker;
bool g_running = false;
bool g_stop = false;
Clock::time_point g_last_asked{};
Clock::time_point g_last_stall{};  // the last kAuto request (game/watchdog.cpp)
// The last automatic screenshot: its picture (a hash) and file.
uint64_t g_last_auto_hash = 0;
std::string g_last_auto_file;

// Held while the picture is read, so Stop can wait for a capture under way.
std::mutex g_capture_mutex;

// The settings, kept here by their change callbacks: the thread below reads
// them without touching the settings' own storage.
std::atomic<bool> g_debug{false};
std::atomic<int> g_interval{60};

bool DetailIsDebug(std::string_view value) { return value == "debug"; }

std::string Stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

const char* Prefix(Kind kind) {
  switch (kind) {
    case Kind::kAsked:
      return "shot";
    case Kind::kAuto:
      return "auto";
    case Kind::kReport:
      return "report";
    case Kind::kCompare:
      return "hd-compare";
  }
  return "shot";
}

// A report's name as the report writer makes it: report-<digits>-<digits>.
// Anything else (it never comes from the player) gives a plain name.
bool IsReportName(std::string_view name) {
  if (name.size() < 8 || name.size() > 48 || name.substr(0, 7) != "report-") return false;
  for (char c : name.substr(7))
    if (!((c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

// Caller holds g_mutex: a file name not on disk and not queued.
std::string UniqueName(const std::string& stem) {
  for (int n = 1; n < 1000; ++n) {
    const std::string name = n == 1 ? stem + ".png" : stem + "-" + std::to_string(n) + ".png";
    std::error_code ec;
    if (std::filesystem::exists(g_folder / name, ec)) continue;
    if (std::any_of(g_jobs.begin(), g_jobs.end(), [&](const Job& j) { return j.file == name; }))
      continue;
    return name;
  }
  return {};
}

// A quick fingerprint of the picture (every 7th pixel), to skip automatic
// screenshots of a picture that hasn't changed (a pause, a stall, a menu).
uint64_t Fingerprint(const rex::ui::RawImage& image) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(image.width);
  mix(image.height);
  for (uint32_t y = 0; y < image.height; y += 3) {
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = y % 7; x < image.width; x += 7) {
      const uint8_t* p = row + size_t(x) * 4;
      mix(uint64_t(p[0]) | uint64_t(p[1]) << 8 | uint64_t(p[2]) << 16);
    }
  }
  return h;
}

bool ValidImage(const rex::ui::RawImage& image) {
  if (!image.width || !image.height || image.width > kMaxSide || image.height > kMaxSide)
    return false;
  if (image.stride < size_t(image.width) * 4) return false;
  return image.data.size() >= image.stride * (image.height - 1) + size_t(image.width) * 4;
}

// Writes the picture as a PNG: to <file>.part first, then renamed, so a
// half-written file never has the real name.
bool WritePng(const rex::ui::RawImage& image, const std::filesystem::path& file, std::string& error) {
  // R, G, B from the capture (its fourth byte is unused), opaque alpha.
  std::vector<uint8_t> rgba(size_t(image.width) * image.height * 4);
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    uint8_t* dst = rgba.data() + size_t(y) * image.width * 4;
    for (uint32_t x = 0; x < image.width; ++x) {
      dst[x * 4 + 0] = src[x * 4 + 0];
      dst[x * 4 + 1] = src[x * 4 + 1];
      dst[x * 4 + 2] = src[x * 4 + 2];
      dst[x * 4 + 3] = 0xFF;
    }
  }
  SDL_Surface* surface = SDL_CreateSurfaceFrom(int(image.width), int(image.height),
                                               SDL_PIXELFORMAT_RGBA32, rgba.data(),
                                               int(image.width * 4));
  if (!surface) {
    error = SDL_GetError();
    return false;
  }
  std::filesystem::path part = file;
  part += ".part";
  const bool ok = SDL_SavePNG(surface, PathToUtf8(part).c_str());
  if (!ok) error = SDL_GetError();
  SDL_DestroySurface(surface);
  std::error_code ec;
  if (!ok) {
    std::filesystem::remove(part, ec);
    return false;
  }
  std::filesystem::rename(part, file, ec);
  if (ec) {
    error = ec.message();
    std::filesystem::remove(part, ec);
    return false;
  }
  return true;
}

// Removes the oldest auto-*.png beyond kAutoKept (only those: the names sort
// by time). Nothing else in the folder is touched.
void PruneAuto(const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> autos;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    const auto& entry = *it;
    std::error_code type_ec;
    if (!entry.is_regular_file(type_ec) || entry.is_symlink(type_ec)) continue;
    const std::string name = PathToUtf8(entry.path().filename());
    if (name.size() > 9 && name.compare(0, 5, "auto-") == 0 &&
        name.compare(name.size() - 4, 4, ".png") == 0)
      autos.push_back(entry.path());
  }
  if (autos.size() <= kAutoKept) return;
  std::sort(autos.begin(), autos.end());
  for (size_t i = 0; i + kAutoKept < autos.size(); ++i) std::filesystem::remove(autos[i], ec);
}

// Captures the picture and writes it as <folder>/<file>, with a log line.
bool Shoot(const std::filesystem::path& folder, const std::string& file, const std::string& reason) {
  rex::ui::RawImage image;
  bool captured = false;
  {
    std::lock_guard capture_lock(g_capture_mutex);
    if (g_capture) captured = g_capture(image);
  }
  std::string error;
  if (captured && !ValidImage(image)) {
    captured = false;
    error = "the picture read back had an unexpected size";
  }
  if (!captured) {
    REXLOG_WARN("cod3: screenshot {} ({}) not taken: {}", file, reason,
                error.empty() ? "no picture from the game yet" : error);
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (!WritePng(image, folder / file, error)) {
    REXLOG_WARN("cod3: screenshot {} ({}) not written: {}", file, reason, error);
    return false;
  }
  REXLOG_INFO("cod3: screenshot userdata/logs/screenshots/{} ({}x{}, {})", file, image.width,
              image.height, reason);
  return true;
}

// The HD comparison (RequestHdCompare). The switch goes back on whatever
// happens, Stop included.
constexpr const char* kSdkReplacementSwitch = "texture_replacement_enabled";
void RunCompare(const Job& job, const std::filesystem::path& folder) {
  if (!Shoot(folder, job.file + "-on.png", "HD textures on")) return;
  if (!rex::cvar::SetFlagByName(kSdkReplacementSwitch, "false")) {
    REXLOG_WARN("cod3: HD compare: the GPU emulation has no {}", kSdkReplacementSwitch);
    return;
  }
  REXLOG_INFO("cod3: HD compare: HD textures off for {:.1f} s", job.seconds_off);
  bool stopping;
  {
    std::unique_lock lock(g_mutex);
    stopping = g_cv.wait_for(lock, std::chrono::duration<double>(job.seconds_off),
                             [] { return g_stop; });
  }
  if (!stopping) Shoot(folder, job.file + "-off.png", "HD textures off");
  rex::cvar::SetFlagByName(kSdkReplacementSwitch, "true");
  REXLOG_INFO("cod3: HD compare: HD textures back on");
}

// The thread: queued screenshots, and the debug log's own.
void Run() {
  // Its name in the thread profiler's table and in debuggers (Linux keeps
  // 15 characters).
#if defined(_WIN32)
  using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  if (auto set_description = reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void*>(
          GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
    set_description(GetCurrentThread(), L"cod3 screenshots");
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), "cod3 screenshot");
#endif
  Clock::time_point last_auto = Clock::now();
  std::unique_lock lock(g_mutex);
  while (!g_stop) {
    g_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop || !g_jobs.empty(); });
    if (g_stop) break;
    Job job;
    if (!g_jobs.empty()) {
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    } else {
      const int interval = g_interval.load();
      if (!g_debug.load() || interval <= 0 ||
          Clock::now() - last_auto < std::chrono::seconds(interval))
        continue;
      job = {Kind::kAuto, "every " + std::to_string(interval) + " s (cod3_debug_screenshots)",
             UniqueName("auto-" + Stamp()), true};
      if (job.file.empty()) continue;
    }
    if (job.kind == Kind::kAuto) last_auto = Clock::now();
    const std::filesystem::path folder = g_folder;
    lock.unlock();
    if (job.kind == Kind::kCompare) {
      RunCompare(job, folder);
      lock.lock();
      continue;
    }

    rex::ui::RawImage image;
    bool captured = false;
    {
      std::lock_guard capture_lock(g_capture_mutex);
      if (g_capture) captured = g_capture(image);
    }
    std::string error;
    bool skip = false;
    std::string same_as;
    if (captured && !ValidImage(image)) {
      captured = false;
      error = "the picture read back had an unexpected size";
    }
    if (captured && job.kind == Kind::kAuto) {
      const uint64_t hash = Fingerprint(image);
      std::lock_guard l(g_mutex);
      if (hash == g_last_auto_hash && !g_last_auto_file.empty()) {
        skip = true;  // the same picture as the last one
        same_as = g_last_auto_file;
      } else {
        g_last_auto_hash = hash;
        g_last_auto_file = job.file;
      }
    }
    if (skip) {
      // Said only for a stall: the periodic ones skip quietly.
      if (!job.periodic)
        REXLOG_INFO("cod3: screenshot ({}): the picture is the same as {}", job.reason, same_as);
    } else if (!captured) {
      // The periodic ones before the game's first picture: nothing to say.
      if (job.periodic && error.empty()) {
        lock.lock();
        continue;
      }
      REXLOG_WARN("cod3: screenshot {} ({}) not taken: {}", job.file, job.reason,
                  error.empty() ? "no picture from the game yet" : error);
    } else {
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      if (WritePng(image, folder / job.file, error)) {
        REXLOG_INFO("cod3: screenshot userdata/logs/screenshots/{} ({}x{}, {})", job.file,
                    image.width, image.height, job.reason);
        if (job.kind == Kind::kAuto) PruneAuto(folder);
      } else {
        REXLOG_WARN("cod3: screenshot {} ({}) not written: {}", job.file, job.reason, error);
      }
    }
    lock.lock();
  }
}

}  // namespace

void Start(const std::filesystem::path& folder, CaptureFn capture) {
  {
    std::lock_guard lock(g_mutex);
    if (g_running || !capture) return;
    g_folder = folder;
    g_stop = false;
    g_running = true;
  }
  {
    std::lock_guard capture_lock(g_capture_mutex);
    g_capture = std::move(capture);
  }
  g_debug = DetailIsDebug(rex::cvar::GetFlagByName("cod3_log_detail"));
  g_interval = REXCVAR_GET(cod3_debug_screenshots);
  rex::cvar::RegisterChangeCallback("cod3_log_detail", [](std::string_view, std::string_view value) {
    g_debug = DetailIsDebug(value);
  });
  rex::cvar::RegisterChangeCallback("cod3_debug_screenshots",
                                    [](std::string_view, std::string_view value) {
                                      g_interval = std::clamp(
                                          std::atoi(std::string(value).c_str()), 0, 3600);
                                    });
  g_worker = std::thread(Run);
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_running) return;
    g_running = false;
    g_stop = true;
    g_jobs.clear();
  }
  g_cv.notify_all();
  {
    // Waits for a capture under way; none starts after this.
    std::lock_guard capture_lock(g_capture_mutex);
    g_capture = nullptr;
  }
  // At most a file being written (no GPU work left).
  if (g_worker.joinable()) g_worker.join();
}

bool Available() {
  std::lock_guard lock(g_mutex);
  return g_running;
}

std::string RequestHdCompare(double seconds_off) {
  std::string stem;
  {
    std::lock_guard lock(g_mutex);
    if (!g_running) return {};
    const auto now = Clock::now();
    if (now - g_last_asked < std::chrono::milliseconds(500)) return {};
    // One at a time: a second would switch the textures back on early.
    if (std::any_of(g_jobs.begin(), g_jobs.end(),
                    [](const Job& j) { return j.kind == Kind::kCompare; }))
      return {};
    g_last_asked = now;
    stem = UniqueName(std::string(Prefix(Kind::kCompare)) + "-" + Stamp());
    if (stem.size() < 5) return {};
    stem.resize(stem.size() - 4);  // without ".png": -on.png and -off.png follow
    Job job;
    job.kind = Kind::kCompare;
    job.reason = "HD textures on and off";
    job.file = stem;
    job.seconds_off = std::clamp(seconds_off, 0.5, 30.0);
    g_jobs.push_back(std::move(job));
  }
  g_cv.notify_all();
  return stem;
}

bool AutoEnabled() { return g_debug.load() && g_interval.load() > 0; }

std::string Request(Kind kind, std::string_view reason, std::string_view name) {
  std::string file;
  {
    std::lock_guard lock(g_mutex);
    if (!g_running) return {};
    // The debug log's own only at debug detail, and a stall's at most once a
    // minute (a slow PC can stall again and again, every few seconds).
    if (kind == Kind::kAuto) {
      if (!g_debug.load()) return {};
      const auto now = Clock::now();
      if (g_last_stall != Clock::time_point{} && now - g_last_stall < std::chrono::seconds(60))
        return {};
      g_last_stall = now;
    }
    if (kind == Kind::kAsked) {
      // Held-down keys and repeated commands: one a half second.
      const auto now = Clock::now();
      if (now - g_last_asked < std::chrono::milliseconds(500)) return {};
      g_last_asked = now;
    }
    if (g_jobs.size() >= 8) return {};
    const std::string stem = kind == Kind::kReport && IsReportName(name)
                                 ? std::string(name)
                                 : std::string(Prefix(kind)) + "-" + Stamp();
    file = UniqueName(stem);
    if (file.empty()) return {};
    g_jobs.push_back({kind, std::string(reason), file});
  }
  g_cv.notify_all();
  return file;
}

}  // namespace cod3::shot
