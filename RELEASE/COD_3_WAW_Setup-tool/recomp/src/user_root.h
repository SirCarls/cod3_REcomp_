// cod3 - the player's files stay in the game's folder (portable), and what happens when it can't take them.
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
// [setup-layout] 2026-11-24 (owner 11:31 "it should be portable."; chose "Both": the setup tool checks first and the game
// never leaves its folder): the game keeps everything in its own folder, always. A folder it can't write ends the
// game with a message that says to move it (Linux here, Windows through ui/start_problem.h LogFileOrStop), instead
// of the 2026-11-16 move to a folder of the player's own (~/.local/share/cod3-recomp, %LOCALAPPDATA%\CoD3-recomp).
// A player whose files went there under that version is told where they are, to copy them back. A user_data_root
// given on the command line still wins (that is the player's own choice).

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

// The folders the 2026-11-16 version moved the player's files to, best first: only looked at now, to say where
// files from that version are.
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

// The folder for userdata/, cod3.toml and cod3mp.toml: always `root` (the game stays portable). When it can't be
// written the game ends here with a message (Linux), or at its log file's check with a box (Windows,
// ui/start_problem.h LogFileOrStop). Call once in OnConfigurePaths, before the log file is chosen.
inline std::filesystem::path Pick(const std::filesystem::path& root) {
  DataRoot() = root;
  if (Writable(root / "userdata" / "logs")) return root;
  // Files the 2026-11-16 version moved out of a read-only game folder: say where, so they can be copied back.
  std::string earlier;
  for (const auto& candidate : FallbackCandidates()) {
    std::error_code ec;
    if (std::filesystem::exists(candidate / "userdata", ec) || std::filesystem::exists(candidate / "cod3.toml", ec)) {
      earlier = PathToUtf8(candidate);
      break;
    }
  }
  Note() = "the game's folder " + PathToUtf8(root) + " can't be written; the game keeps its settings, saves and log "
           "in its own folder, so move it to a folder of your own" +
           (earlier.empty() ? std::string() : " (settings and saves from an earlier version are in " + earlier + ")");
#ifndef _WIN32  // platform-ok(os-only-block): Windows' stop is LogFileOrStop's box (ui/start_problem.h), see below
  // Linux: say why and end, instead of spdlog's uncaught error (SIGABRT, words on stderr only).
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                            "Call of Duty 3 keeps its settings, saves and log in its own folder, and this folder\n" +
                                PathToUtf8(root) +
                                "\ncan't be written. Move the game to a folder of your own (in your home folder), or "
                                "check the disk (full, or mounted read-only)." +
                                (earlier.empty() ? std::string()
                                                 : "\n\nSettings and saves from an earlier version are in\n" + earlier +
                                                       "\nCopy its userdata folder and .toml files into the game's folder."));
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
