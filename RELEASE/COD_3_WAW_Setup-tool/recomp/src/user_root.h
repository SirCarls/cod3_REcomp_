// cod3 - where the player's files go when the game's folder can't take them.
// [rexglue-native-io] 2026-11-16 (owner 2026-11-16 14:16: "Start looking for more I/O related errors, and account
// for various linux distros and windows").
//
// The port keeps everything in one folder (Cod3App::OnConfigurePaths): <root>/userdata (saves, the console config,
// logs, shader cache) and <root>/cod3.toml, cod3mp.toml (the settings). When that folder can't be written:
//  - Linux: the log file couldn't be opened and the game ended before its window, with nothing but a line on stderr
//    (spdlog's "Failed opening file ... for writing", std::terminate). A folder the user can't write: installed by
//    root (/opt, /usr/local), a read-only mount (ISO, NFS/SMB share, a disk mounted read-only after an error),
//    a copied folder with read-only permissions, an exFAT/NTFS mount owned by another uid.
//  - Windows: a message box ("can't write its log file", ui/start_problem.h) and the end of the program. Program
//    Files without administrator rights (no file virtualization for a 64-bit program), Controlled folder access on
//    Documents/Desktop, a read-only share.
// Here such a game keeps running and its files go to a folder of the player's own instead:
//    Linux    $XDG_DATA_HOME/cod3-recomp   (~/.local/share/cod3-recomp), else $XDG_RUNTIME_DIR, else /tmp
//    Windows  %LOCALAPPDATA%\CoD3-recomp   (never synced by OneDrive, not under Controlled folder access)
// with the same layout (userdata/, cod3.toml, cod3mp.toml). On its first use it takes the game folder's settings
// files and userdata (saves, configs; not logs or caches), so nothing the player had is lost. Both programs pick the
// same folder, so the campaign and multiplayer still share their settings. A game folder that can be written is used
// as before (nothing changes for it), and a user_data_root setting still wins over both.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <rex/system.h>  // ShowSimpleMessageBox

#include "util.h"

namespace cod3::user_root {

// The note for the log (empty when the game folder is used), set by Pick, said by LogNote once logging is up.
inline std::string& Note() {
  static std::string note;
  return note;
}

// The folder Pick chose (the game folder until then): multiplayer reads the campaign's cod3.toml and userdata there.
inline std::filesystem::path& DataRoot() {
  static std::filesystem::path root;
  return root;
}

// Whether files can be made in `dir` (made first if missing): a probe file is written and removed.
inline bool Writable(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec)) return false;
#ifdef _WIN32
  const auto probe = dir / (".write-test-" + std::to_string(GetCurrentProcessId()));
#else
  const auto probe = dir / (".write-test-" + std::to_string(getpid()));
#endif
  bool ok = false;
  {
    std::ofstream out(probe, std::ios::binary | std::ios::trunc);
    if (out) {
      out << "x";
      out.flush();
      ok = static_cast<bool>(out);
    }
  }
  std::filesystem::remove(probe, ec);
  return ok;
}

// The player's own folders to fall back to, best first.
inline std::vector<std::filesystem::path> FallbackCandidates() {
  std::vector<std::filesystem::path> out;
#ifdef _WIN32
  // The environment's names (set for every Windows 10/11 session; no shell32/uuid link needed for them).
  if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && *local)
    out.push_back(std::filesystem::path(local) / L"CoD3-recomp");
  if (const wchar_t* temp = _wgetenv(L"TEMP"); temp && *temp)
    out.push_back(std::filesystem::path(temp) / L"CoD3-recomp");
#else
  auto env = [](const char* name) -> std::filesystem::path {
    const char* v = std::getenv(name);
    return v && *v == '/' ? std::filesystem::path(v) : std::filesystem::path();  // XDG: absolute paths only
  };
  if (const auto xdg = env("XDG_DATA_HOME"); !xdg.empty()) out.push_back(xdg / "cod3-recomp");
  if (const auto home = env("HOME"); !home.empty()) out.push_back(home / ".local" / "share" / "cod3-recomp");
  if (const auto run = env("XDG_RUNTIME_DIR"); !run.empty()) out.push_back(run / "cod3-recomp");
  out.push_back(std::filesystem::path("/tmp") / ("cod3-recomp-" + std::to_string(getuid())));
#endif
  return out;
}

// Copies what the player had in the game folder into a new fallback folder: the settings files and userdata
// without logs/ and cache/. Errors are skipped (the game then starts from defaults for that file).
inline void Seed(const std::filesystem::path& root, const std::filesystem::path& to) {
  std::error_code ec;
  for (const char* name : {"cod3.toml", "cod3mp.toml"})
    if (std::filesystem::is_regular_file(root / name, ec) && !std::filesystem::exists(to / name, ec))
      std::filesystem::copy_file(root / name, to / name, ec);
  const auto from = root / "userdata";
  if (!std::filesystem::is_directory(from, ec)) return;
  for (std::filesystem::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename();
    if (name == "logs" || name == "cache") continue;
    std::error_code one;
    std::filesystem::copy(it->path(), to / "userdata" / name,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing, one);
  }
  // The copies keep the originals' modes (a read-only folder's files are read-only too, and Windows copies the
  // read-only attribute): the player's own copies must take the game's saves.
  const auto writable = [](const std::filesystem::path& p) {
    std::error_code one;
    std::filesystem::permissions(p, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, one);
  };
  for (const char* name : {"cod3.toml", "cod3mp.toml"}) writable(to / name);
  for (std::filesystem::recursive_directory_iterator it(to / "userdata", ec), end; !ec && it != end; it.increment(ec))
    writable(it->path());
}

// The folder for userdata/, cod3.toml and cod3mp.toml: `root` when it can be written, else the player's own (made
// and seeded on first use). Call once in OnConfigurePaths, before the log file is chosen.
inline std::filesystem::path Pick(const std::filesystem::path& root) {
  DataRoot() = root;
  if (Writable(root / "userdata" / "logs")) return root;
  for (const auto& candidate : FallbackCandidates()) {
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(candidate / "userdata", ec);
    if (!Writable(candidate / "userdata" / "logs")) continue;
    if (fresh) Seed(root, candidate);
    DataRoot() = candidate;
    Note() = "the game's folder " + PathToUtf8(root) +
             " can't be written (read-only, or a folder such as Program Files or one Windows Security protects), so "
             "settings, saves and logs are kept in " + PathToUtf8(candidate) +
             (fresh ? " (its settings and saves were copied there)" : "") +
             ". Move the game to a folder of your own to keep them next to it.";
    return candidate;
  }
  Note() = "neither the game's folder " + PathToUtf8(root) + " nor a folder of the player's own can be written";
#ifndef _WIN32  // platform-ok(os-only-block): Windows' stop is LogFileOrStop's box (ui/start_problem.h), see below
  // Linux: say why and end, instead of spdlog's uncaught error (SIGABRT, words on stderr only). Windows: the log's
  // check (ui/start_problem.h LogFileOrStop) shows its own box for this.
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                            "Call of Duty 3 can't write its settings, saves and log: neither its folder\n" +
                                PathToUtf8(root) +
                                "\nnor a folder of your own (~/.local/share, $XDG_RUNTIME_DIR, /tmp) can be written. "
                                "Move the game to a folder of your own, or check the disk (full, or mounted read-only).");
  std::fprintf(stderr, "cod3: %s\n", Note().c_str());
  std::exit(1);
#endif
  return root;  // Windows: LogFileOrStop says why in a box and ends
}

// The note for the log, once (empty if there is none): call after logging started and say it as a warning, so
// F2 > Console > Problems shows it too.
inline std::string TakeNote() {
  std::string note;
  note.swap(Note());
  return note;
}

}  // namespace cod3::user_root
