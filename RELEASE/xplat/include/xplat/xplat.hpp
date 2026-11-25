// xplat for C++: std::filesystem::path and UTF-8 text, the one way the project converts between them, plus the
// clock, sleeps and the player's folders of xplat.h. [rexglue-native-platform-fix] 2026-11-23
//
// Rules this header makes easy to follow (dev/agents/PLATFORM-CHECKS.md §4):
//  - inside a program a path is a std::filesystem::path or UTF-8 text; convert only with PathFromUtf8 / Utf8FromPath;
//  - never path.string() for a path a user can name (Windows: the ANSI code page, and the C++ library throws for a
//    letter outside it), never std::filesystem::path(std::string) for UTF-8 text (a setting, the command line, a
//    TOML value: read as the ANSI code page on Windows);
//  - open files by the path itself (std::ifstream/ofstream take a path; on Windows they open by its UTF-16 form).
#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "xplat/xplat.h"

namespace xplat {

namespace detail {
// path::u8string() is std::string in C++17 and std::u8string in C++20; both become plain UTF-8 bytes here.
inline std::string AsChars(const std::string& s) { return s; }
#if defined(__cpp_lib_char8_t)
inline std::string AsChars(const std::u8string& s) {
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
#endif
}  // namespace detail

// The path for UTF-8 text (a setting's value, a command-line argument, a TOML string). Through std::u8string, so on
// Windows every letter is kept whatever the ANSI code page is.
inline std::filesystem::path PathFromUtf8(std::string_view text) {
#if defined(__cpp_lib_char8_t)  // C++20: through char8_t text
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else                           // C++17 (or -fno-char8_t): u8path, which C++20 deprecates
  return std::filesystem::u8path(text.begin(), text.end());
#endif
}

// UTF-8 text for a path, with the system's separators (for logs and messages). u8string() never throws, unlike
// string() on Windows.
inline std::string Utf8FromPath(const std::filesystem::path& p) {
  return detail::AsChars(p.u8string());
}

// UTF-8 text for a path with "/" on every system, for a path stored in a setting or a TOML file ("C:\Users\<name>" is
// not valid TOML: "\U" starts an escape). Windows accepts "/" in every path.
inline std::string GenericUtf8FromPath(const std::filesystem::path& p) {
  return detail::AsChars(p.generic_u8string());
}

enum class UserDirKind { kConfig = XPLAT_USER_CONFIG, kData = XPLAT_USER_DATA, kCache = XPLAT_USER_CACHE,
                         kState = XPLAT_USER_STATE };

// The player's own folder of `kind` for the program `app` (xplat_user_dir), or an empty path when the system has
// none. Not made. For new code and announced moves only: a game's existing files stay where they are.
inline std::filesystem::path UserDir(UserDirKind kind, std::string_view app) {
  const std::string name(app);
  std::string buf(32768, '\0');  // on the heap: a game thread may have a small stack
  if (xplat_user_dir(static_cast<xplat_user_dir_kind>(kind), name.c_str(), buf.data(), buf.size()) != 0) return {};
  buf.resize(std::char_traits<char>::length(buf.c_str()));
  return PathFromUtf8(buf);
}

// A clock that only goes forward (xplat_monotonic_ns), as a std::chrono duration since an unspecified start.
inline std::chrono::nanoseconds MonotonicNow() {
  return std::chrono::nanoseconds(static_cast<std::int64_t>(xplat_monotonic_ns()));
}

// Sleeps until MonotonicNow() reaches `deadline` (xplat_sleep_until_ns: a high-resolution timer on Windows,
// clock_nanosleep on Linux).
inline void SleepUntil(std::chrono::nanoseconds deadline) {
  if (deadline.count() > 0) xplat_sleep_until_ns(static_cast<std::uint64_t>(deadline.count()));
}

template <class Rep, class Period>
inline void SleepFor(std::chrono::duration<Rep, Period> d) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
  if (ns > 0) xplat_sleep_ns(static_cast<std::uint64_t>(ns));
}

// Renames `from` over `to` in one step where the system can (xplat_rename_replace: Windows retries briefly while an
// antivirus or the indexer holds the file). The safe save: write "<name>.tmp", then ReplaceFile it over "<name>".
inline bool ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
  if (xplat_rename_replace(Utf8FromPath(from).c_str(), Utf8FromPath(to).c_str()) == 0) {
    ec.clear();
    return true;
  }
  ec = std::error_code(errno, std::generic_category());
  return false;
}

}  // namespace xplat
