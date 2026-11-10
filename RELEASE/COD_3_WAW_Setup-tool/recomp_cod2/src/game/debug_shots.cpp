// cod2 - pictures of the game for test runs (see debug_shots.h). [acct2-cod2]

#include "game/debug_shots.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_INT32(cod2_debug_shot_seconds, 0, "cod2/Debug",
                     "Tests: every this many seconds, measure the game's picture into the log and "
                     "save it when it changed (userdata/logs/screenshots; 0 = never)")
    .range(0, 3600);

namespace cod2::shots {

namespace {

constexpr int kMaxFiles = 50;

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;

struct Measure {
  double mean_luma = 0;      // 0-255
  double lit_share = 0;      // pixels with luma above 24, 0-1
  size_t coarse_colors = 0;  // distinct colors at 3 bits a channel
  uint64_t fingerprint = 0;
};

Measure MeasureImage(const rex::ui::RawImage& image) {
  Measure m;
  uint64_t h = 1469598103934665603ull;
  std::unordered_set<uint32_t> colors;
  double sum = 0;
  size_t lit = 0, count = 0;
  for (uint32_t y = 0; y < image.height; y += 2) {
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; x += 2) {
      const uint8_t r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
      const double luma = 0.299 * r + 0.587 * g + 0.114 * b;
      sum += luma;
      lit += luma > 24;
      ++count;
      colors.insert(uint32_t(r >> 5) << 6 | uint32_t(g >> 5) << 3 | uint32_t(b >> 5));
      h = (h ^ (uint32_t(r) << 16 | uint32_t(g) << 8 | b)) * 1099511628211ull;
    }
  }
  if (count) {
    m.mean_luma = sum / count;
    m.lit_share = double(lit) / count;
  }
  m.coarse_colors = colors.size();
  m.fingerprint = h;
  return m;
}

bool WritePpm(const rex::ui::RawImage& image, const std::filesystem::path& file) {
  std::ofstream out(file, std::ios::binary);
  if (!out) return false;
  out << "P6\n" << image.width << " " << image.height << "\n255\n";
  std::string row(size_t(image.width) * 3, '\0');
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 3] = char(src[x * 4]);
      row[x * 3 + 1] = char(src[x * 4 + 1]);
      row[x * 3 + 2] = char(src[x * 4 + 2]);
    }
    out.write(row.data(), std::streamsize(row.size()));
  }
  return bool(out);
}

void Run(std::filesystem::path folder, CaptureFn capture, int seconds) {
  uint64_t last = 0;
  int files = 0, n = 0;
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    {
      std::unique_lock lock(g_mutex);
      if (g_cv.wait_for(lock, std::chrono::seconds(seconds), [] { return g_stop; })) return;
    }
    ++n;
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    rex::ui::RawImage image;
    if (!capture(image) || !image.width || !image.height ||
        image.stride < size_t(image.width) * 4 ||
        image.data.size() < image.stride * (image.height - 1) + size_t(image.width) * 4) {
      REXLOG_INFO("cod2: picture {} at {:.0f} s: none yet", n, t);
      continue;
    }
    const Measure m = MeasureImage(image);
    std::string saved;
    if (m.fingerprint != last && files < kMaxFiles) {
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      const std::string name = "auto-" + std::to_string(n) + ".ppm";
      if (WritePpm(image, folder / name)) {
        saved = ", saved " + name;
        ++files;
      }
    }
    REXLOG_INFO("cod2: picture {} at {:.0f} s: {}x{}, mean brightness {:.1f}, {:.1f}% lit, {} coarse "
                "colors, fingerprint {:016X}{}{}",
                n, t, image.width, image.height, m.mean_luma, 100 * m.lit_share, m.coarse_colors,
                m.fingerprint, m.fingerprint == last ? " (unchanged)" : "", saved);
    last = m.fingerprint;
  }
}

}  // namespace

void Start(const std::filesystem::path& folder, CaptureFn capture) {
  const int seconds = REXCVAR_GET(cod2_debug_shot_seconds);
  if (seconds <= 0 || g_thread.joinable()) return;
  g_stop = false;
  g_thread = std::thread(Run, folder, std::move(capture), seconds);
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace cod2::shots
