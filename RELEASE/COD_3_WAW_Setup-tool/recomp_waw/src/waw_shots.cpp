// waw - debug screenshots (waw_shots.h). [acct2-waw]

#include "waw_shots.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_INT32(waw_debug_screenshots, 0, "waw",
                     "Debug: every this many seconds, save the game's own picture to userdata/logs/screenshots and "
                     "log its brightness by area (0 = off)");

namespace waw::shots {
namespace {

constexpr size_t kKept = 30;          // the newest auto-*.ppm kept
constexpr uint32_t kMaxSide = 16384;  // larger captures are taken as broken
constexpr int kGrid = 4;              // the brightness grid is kGrid x kGrid cells

std::mutex g_mutex;
std::condition_variable g_wake;
bool g_stop = false;
std::thread g_thread;

bool Valid(const rex::ui::RawImage& image) {
  if (!image.width || !image.height || image.width > kMaxSide || image.height > kMaxSide) return false;
  if (image.stride < size_t(image.width) * 4) return false;
  return image.data.size() >= image.stride * (image.height - 1) + size_t(image.width) * 4;
}

// Brightness 0-255 of an R8 G8 B8 X8 pixel (Rec. 601 weights).
uint32_t Luma(const uint8_t* p) { return (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8; }

std::string Measure(const rex::ui::RawImage& image) {
  uint64_t total = 0, lit = 0;
  uint64_t cell_sum[kGrid][kGrid] = {};
  uint64_t cell_count[kGrid][kGrid] = {};
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    const int cy = int(uint64_t(y) * kGrid / image.height);
    for (uint32_t x = 0; x < image.width; ++x) {
      const uint32_t l = Luma(row + size_t(x) * 4);
      const int cx = int(uint64_t(x) * kGrid / image.width);
      total += l;
      lit += l > 16;
      cell_sum[cy][cx] += l;
      ++cell_count[cy][cx];
    }
  }
  const uint64_t pixels = uint64_t(image.width) * image.height;
  std::string grid;
  for (int cy = 0; cy < kGrid; ++cy) {
    if (cy) grid += " /";
    for (int cx = 0; cx < kGrid; ++cx)
      grid += " " + std::to_string(cell_count[cy][cx] ? cell_sum[cy][cx] / cell_count[cy][cx] : 0);
  }
  char head[96];
  std::snprintf(head, sizeof(head), "%ux%u, mean %.1f, not black %.1f%%, grid", image.width, image.height,
                double(total) / double(pixels), 100.0 * double(lit) / double(pixels));
  return head + grid;
}

bool WritePpm(const rex::ui::RawImage& image, const std::filesystem::path& file) {
  std::filesystem::path part = file;
  part += ".part";
  FILE* f = std::fopen(part.string().c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
  std::vector<uint8_t> row(size_t(image.width) * 3);
  bool ok = true;
  for (uint32_t y = 0; y < image.height && ok; ++y) {
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 3 + 0] = src[x * 4 + 0];
      row[x * 3 + 1] = src[x * 4 + 1];
      row[x * 3 + 2] = src[x * 4 + 2];
    }
    ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
  }
  ok = (std::fclose(f) == 0) && ok;
  std::error_code ec;
  if (ok) std::filesystem::rename(part, file, ec);
  if (!ok || ec) {
    std::filesystem::remove(part, ec);
    return false;
  }
  return true;
}

// Removes the oldest auto-*.ppm beyond kKept (the names sort by their number).
void Prune(const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("auto-", 0) == 0 && entry.path().extension() == ".ppm") files.push_back(entry.path());
  }
  if (files.size() <= kKept) return;
  std::sort(files.begin(), files.end());
  for (size_t i = 0; i + kKept < files.size(); ++i) std::filesystem::remove(files[i], ec);
}

void Run(std::filesystem::path folder, CaptureFn capture, int seconds) {
  const auto start = std::chrono::steady_clock::now();
  unsigned number = 0;
  std::unique_lock<std::mutex> lock(g_mutex);
  while (!g_wake.wait_for(lock, std::chrono::seconds(seconds), [] { return g_stop; })) {
    lock.unlock();
    const long at = long(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start)
                             .count());
    rex::ui::RawImage image;
    if (!capture(image) || !Valid(image)) {
      REXLOG_INFO("waw: screenshot at {} s: no picture yet", at);
    } else {
      char name[32];
      std::snprintf(name, sizeof(name), "auto-%04u.ppm", ++number);
      const bool saved = WritePpm(image, folder / name);
      REXLOG_INFO("waw: screenshot at {} s{}: {}", at, saved ? std::string(" ") + name : std::string(" (not saved)"),
                  Measure(image));
      if (saved) Prune(folder);
    }
    lock.lock();
  }
}

}  // namespace

void Start(const std::filesystem::path& folder, CaptureFn capture) {
  const int seconds = REXCVAR_GET(waw_debug_screenshots);
  if (seconds <= 0 || g_thread.joinable()) return;
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  g_stop = false;
  g_thread = std::thread(Run, folder, std::move(capture), seconds);
  REXLOG_INFO("waw: debug screenshots every {} s", seconds);
}

void Stop() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = true;
  }
  g_wake.notify_all();
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace waw::shots
