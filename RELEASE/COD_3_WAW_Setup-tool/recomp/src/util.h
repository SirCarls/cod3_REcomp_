// cod3 - small host-side helpers.

#pragma once

#include <filesystem>
#include <string>

namespace cod3 {

// UTF-8 text for a path. path::string() uses the ANSI code page on Windows and
// can mangle or throw on non-ASCII user names; u8string() never does.
inline std::string PathToUtf8(const std::filesystem::path& p) {
  auto u8 = p.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// UTF-8 text for a path with forward slashes on every system, for a path
// stored in a setting. F2's Save writes every changed setting into cod3.toml
// without escaping, and a Windows path such as "C:\Users\<name>\..." there is not
// valid TOML ("\U" starts an escape), so the whole file would fail to load.
// Windows accepts "/" as well.
inline std::string PathToUtf8Generic(const std::filesystem::path& p) {
  auto u8 = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// The path for UTF-8 text (the reverse of PathToUtf8).
inline std::filesystem::path Utf8ToPath(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}  // namespace cod3
