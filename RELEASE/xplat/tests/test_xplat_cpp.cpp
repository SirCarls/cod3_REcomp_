// xplat's tests (C++: xplat.hpp with std::filesystem). [rexglue-native-platform-fix] 2026-11-23
// Built as C++20 by xplat's CMakeLists and, with XPLAT_TEST_CXX17, as C++17 too: the header must work in both.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "xplat/xplat.hpp"

namespace fs = std::filesystem;

static int failures = 0, checks = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    checks++;                                                                    \
    if (!(cond)) {                                                               \
      failures++;                                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)

// "Łódź-日本-ü-🎮" as UTF-8 bytes (see test_xplat.c)
static const char kName[] = "\xC5\x81\xC3\xB3" "d" "\xC5\xBA" "-\xE6\x97\xA5\xE6\x9C\xAC-\xC3\xBC-\xF0\x9F\x8E\xAE";

static std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);  // opens by the path itself: UTF-16 on Windows
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void WriteAll(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary);
  out << text;
}

int main() {
  // UTF-8 <-> path, both ways, every letter kept
  const fs::path name = xplat::PathFromUtf8(kName);
  CHECK(xplat::Utf8FromPath(name) == kName);
  CHECK(xplat::GenericUtf8FromPath(name) == kName);
#if defined(_WIN32)
  CHECK(name.native() == L"Łódź-日本-ü-\U0001F3AE");
  CHECK(xplat::GenericUtf8FromPath(xplat::PathFromUtf8("a\\b/c")) == "a/b/c");
  CHECK(xplat::Utf8FromPath(xplat::PathFromUtf8("a/b").make_preferred()) == "a\\b");
#else
  CHECK(name.native() == kName);
#endif
  CHECK(xplat::Utf8FromPath(xplat::PathFromUtf8("")).empty());

  // std::filesystem and xplat see the same names
  const fs::path root = fs::path("xplat_tmp_cpp") / name;
  std::error_code ec;
  fs::remove_all(fs::path("xplat_tmp_cpp"), ec);
  CHECK(fs::create_directories(root / "sub", ec) && !ec);
  xplat_stat_t st;
  CHECK(xplat_stat(xplat::Utf8FromPath(root / "sub").c_str(), &st) == 0 && st.is_dir);
  CHECK(xplat_mkdirs(xplat::Utf8FromPath(root / "made" / name).c_str()) == 0);
  CHECK(fs::is_directory(root / "made" / name));

  const fs::path from = root / xplat::PathFromUtf8(std::string(kName) + ".new");  // never fs::path(std::string)
  const fs::path to = root / "settings.toml";
  WriteAll(to, "old");
  WriteAll(from, "new");
  CHECK(xplat::ReplaceFile(from, to, ec) && !ec);
  CHECK(ReadAll(to) == "new");
  CHECK(!fs::exists(from));
  CHECK(!xplat::ReplaceFile(root / "missing", to, ec) && ec == std::errc::no_such_file_or_directory);

  // the player's folders
  const fs::path config = xplat::UserDir(xplat::UserDirKind::kConfig, "App");
  CHECK(!config.empty() && config.is_absolute() && config.filename() == "App");
#if defined(_WIN32)
  const fs::path cache = xplat::UserDir(xplat::UserDirKind::kCache, "App");
  CHECK(cache.filename() == "cache" && cache.parent_path().filename() == "App");
#endif
  CHECK(xplat::UserDir(xplat::UserDirKind::kData, "../bad").empty());
  std::printf("  config folder: %s\n", xplat::Utf8FromPath(config).c_str());

  // the clock and sleeps
  using namespace std::chrono_literals;
  const auto t0 = xplat::MonotonicNow();
  xplat::SleepFor(3ms);
  const auto t1 = xplat::MonotonicNow();
  CHECK(t1 - t0 >= 3ms);
  CHECK(t1 - t0 < 250ms);
  xplat::SleepUntil(t1 - 1ms);  // in the past
  xplat::SleepFor(-5ms);        // nothing
  CHECK(xplat::MonotonicNow() - t1 < 5ms);

  fs::remove_all(fs::path("xplat_tmp_cpp"), ec);
  CHECK(!fs::exists(fs::path("xplat_tmp_cpp")));

  std::printf("%s: %d checks, %d failed (C++%ld)\n", failures ? "FAILED" : "ok", checks, failures,
              static_cast<long>(__cplusplus));
  return failures ? 1 : 0;
}
