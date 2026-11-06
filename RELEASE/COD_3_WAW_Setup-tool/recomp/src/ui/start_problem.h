// cod3 - Windows: a start that fails says why ([acct2-decomp]; owner 2026-11-06
// 02:06: "Ye this needs to be fixed ASAP"). Both programs (cod3_app.cpp,
// cod3mp_app.cpp); nothing here is built on Linux.
//
// A Windows program has no console. Bug reports came without a log: a start
// that failed before the log file was open ended without a word.
//  - The log file's name. The SDK's log (spdlog) opens it by a narrow name,
//    which Windows reads in the system's code page ("language for non-Unicode
//    programs"). The campaign passed path::generic_string(), which throws for a
//    character that code page doesn't have (a user or folder name in another
//    script); multiplayer passed UTF-8, which Windows reads as other characters
//    for anything not ASCII, so its log went to a wrong folder or failed to
//    open. LogFileOrStop gives a name that reaches the folder (its short 8.3
//    name when the folder's own name isn't plain ASCII), checks the file opens
//    as the log opens it, and otherwise says what to do and ends the program.
//  - Install: the program's name on message boxes (SDK patch 0183) and a last
//    word for an error nothing caught on the start's thread (std::terminate;
//    MSVC keeps one handler per thread, llvm-mingw's libc++ one for all).
//    The SDK (0183) also says why for a start that fails before this code.
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <share.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#include <rex/system.h>

#include "util.h"

namespace cod3::start_problem {

struct Program {
  const char* title = "Call of Duty 3";  // message box title
  const char* exe = "cod3.exe";
  const char* log = "cod3.log";
};

inline Program& Current() {
  static Program program;
  return program;
}

inline bool IsAscii(const std::wstring& text) {
  for (const wchar_t c : text)
    if (c < 0x20 || c > 0x7E) return false;
  return true;
}

// The text in the system's code page, as the C runtime's narrow file functions
// read it; "" if some character has no exact match there (no best-fit
// stand-in, such as a plain e for an accented one, which would name another
// folder).
inline std::string ToCodePage(const std::wstring& text) {
  if (text.empty()) return {};
  const UINT code_page = GetACP();
  const bool utf8 = code_page == CP_UTF8;  // the "Beta: use Unicode UTF-8" option
  const DWORD flags = utf8 ? 0 : WC_NO_BEST_FIT_CHARS;
  BOOL lossy = FALSE;
  const int size = WideCharToMultiByte(code_page, flags, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, utf8 ? nullptr : &lossy);
  if (size <= 0 || lossy) return {};
  std::string narrow(static_cast<size_t>(size), '\0');
  if (WideCharToMultiByte(code_page, flags, text.data(), static_cast<int>(text.size()), narrow.data(),
                          size, nullptr, utf8 ? nullptr : &lossy) != size ||
      lossy)
    return {};
  return narrow;
}

[[noreturn]] inline void Stop(const std::string& message, UINT exit_code) {
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, message);
  TerminateProcess(GetCurrentProcess(), exit_code);
  std::_Exit(static_cast<int>(exit_code));
}

// The narrow name the SDK's log opens for log_path (forward slashes; its
// folder must exist), or "" if none reaches it.
inline std::string LogFileName(const std::filesystem::path& log_path) {
  std::wstring dir = log_path.parent_path().wstring();
  if (!IsAscii(dir)) {
    // The short name is plain ASCII where the drive keeps short names (the
    // system drive does unless that was turned off).
    const DWORD size = GetShortPathNameW(dir.c_str(), nullptr, 0);
    if (size > 0) {
      std::wstring short_dir(size, L'\0');
      const DWORD got = GetShortPathNameW(dir.c_str(), short_dir.data(), size);
      if (got > 0 && got < size) {
        short_dir.resize(got);
        if (IsAscii(short_dir)) dir = short_dir;
      }
    }
  }
  // Slashes before converting: in an East Asian code page a character's second
  // byte can be '\' (0x5C).
  std::wstring name = dir + L"/" + log_path.filename().wstring();
  for (wchar_t& c : name)
    if (c == L'\\') c = L'/';
  return ToCodePage(name);
}

// What the log opens (spdlog: _fsopen(name, "ab", _SH_DENYNO)) opens here.
inline bool LogOpens(const std::string& name) {
  std::FILE* file = _fsopen(name.c_str(), "ab", _SH_DENYNO);
  if (!file) return false;
  std::fclose(file);
  return true;
}

// The log_file setting for log_path, or a message box and the end of the
// program. Its folder is made first (create_directories).
inline std::string LogFileOrStop(const std::filesystem::path& log_path) {
  const Program& program = Current();
  const std::string shown = cod3::PathToUtf8(log_path.lexically_normal().make_preferred());
  const std::string name = LogFileName(log_path);
  if (name.empty())
    Stop(std::string(program.title) + " can't make its log file:\n\n" + shown +
             "\n\nThe folder's path has a character that this PC's language setting for "
             "non-Unicode programs doesn't have. Move the game's folder to a plain path such as "
             "C:\\Games\\COD3 and start it again.",
         1);
  if (!LogOpens(name))
    Stop(std::string(program.title) + " can't write its log file:\n\n" + shown +
             "\n\nThe folder may be read-only or inside Program Files, or Windows Security "
             "(Controlled folder access) or an antivirus may be blocking it. Move the game's "
             "folder to a plain path such as C:\\Games\\COD3, or allow " + program.exe +
             " in Windows Security, then start it again.",
         1);
  return name;
}

// The exception std::terminate was called for, as UTF-8.
inline std::string TerminateReason() {
  const std::exception_ptr error = std::current_exception();
  if (!error) return "the program ended itself (std::terminate)";
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& e) {
    const char* what = e.what();
    if (!what || !*what) return "an error without a description";
    // what() is often in the system's code page (a path): UTF-8 for the box.
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, what, -1, nullptr, 0) > 0) return what;
    const int size = MultiByteToWideChar(CP_ACP, 0, what, -1, nullptr, 0);
    if (size <= 1) return what;
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, what, -1, wide.data(), size);
    return cod3::PathToUtf8(std::filesystem::path(wide));
  } catch (...) {
  }
  return "an error without a description";
}

[[noreturn]] inline void EndWithMessage() {
  const Program& program = Current();
  Stop(std::string(program.title) + " stopped because of an error:\n\n" + TerminateReason() +
           "\n\nIts log file, if one was made: userdata\\logs\\" + program.log + " in the game's folder.",
       3);
}

// First thing in OnConfigurePaths.
inline void Install(const char* title, const char* exe, const char* log) {
  Current() = Program{title, exe, log};
  rex::SetSimpleMessageBoxTitle(title);
  std::set_terminate(EndWithMessage);
}

}  // namespace cod3::start_problem
#endif  // _WIN32
