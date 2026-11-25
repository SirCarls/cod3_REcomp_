# xplat: the project's platform layer

[rexglue-native-platform-fix] 2026-11-23. One small library, shared by every game in the project (Call of Duty 3,
the other Xbox 360 ports, Big Red One, Finest Hour), for the things that differ between Windows and Linux and
that each game used to do its own way: file names outside ASCII, long paths, the command line, clocks and sleeps,
the player's folders, and the Windows manifest and Vulkan pieces of the build.

## The rule

**New code uses xplat for file paths, clocks and sleeps.** That means:

| Instead of                                                     | use                                             |
|----------------------------------------------------------------|-------------------------------------------------|
| `fopen`, `stat`, `mkdir`, `rename`, `remove`, `opendir` on a `char*` path | `xplat_fopen`, `xplat_stat`, `xplat_mkdir(s)`, `xplat_rename_replace`, `xplat_remove`, `xplat_opendir` |
| `std::filesystem::path(std::string)`, `path.string()`          | `xplat::PathFromUtf8`, `xplat::Utf8FromPath`    |
| `main`'s `argv` on Windows                                     | `xplat_utf8_args(&argc, &argv)` first thing     |
| `getenv`                                                       | `xplat_getenv`                                  |
| `time()`, `GetTickCount`, `GetSystemTime*`, `timeGetTime` for intervals | `xplat_monotonic_ns` / `xplat::MonotonicNow` |
| `Sleep`, `usleep`, `nanosleep`, `std::this_thread::sleep_*` for pacing | `xplat_sleep_until_ns` / `xplat::SleepUntil` |
| `timeBeginPeriod(1)` scattered around                          | `xplat_request_fine_timer()` once               |
| a hand-made `%APPDATA%` / `~/.config` path for **new** data    | `xplat_user_dir`                                |
| `app.rc` + manifest written per game                           | `xplat_windows_app(<target> [GUI] [WMAIN])`     |
| a per-game ngl_vk / Vulkan headers / glslc search              | `xplat_find_ngl_vk(<PREFIX> TARGET <t> ...)`    |

In new code the left-hand forms are platform findings; the right-hand ones are the fix.

**Adopting xplat never moves a player's files.** Wherever a game reads and writes today (settings, saves, logs,
mods) stays where it is; `xplat_user_dir` is for new data, or for a move that is announced and migrates the old
folder. A frame limiter that moves to `xplat_sleep_until_ns` is measured at 20/30/60/144 fps and uncapped
before and after.

## What it does on each system

Every string is UTF-8. Functions return 0, or -1 with `errno` set.

| | Windows | Linux (POSIX) |
|---|---|---|
| paths | converted to UTF-16 and given to the wide functions (`_wfopen`, `CreateFileW`, `MoveFileExW`...). From 248 characters on, made full with `GetFullPathNameW` (so `/`, `.`, `..` and relative paths still work) and passed as `\\?\C:\...` or `\\?\UNC\server\share\...` | the bytes as they are; `fopen` gets `e` (close-on-exec) |
| `xplat_rename_replace` | `MoveFileExW(REPLACE_EXISTING \| WRITE_THROUGH)`, retried for ~0.2 s while a scanner holds the file | `rename` |
| `xplat_utf8_args` | `CommandLineToArgvW`, converted once | nothing to do |
| `xplat_monotonic_ns` | `QueryPerformanceCounter` | `CLOCK_MONOTONIC` |
| `xplat_sleep_until_ns` | a high-resolution waitable timer (Windows 10 1803+), else `Sleep` | `clock_nanosleep(TIMER_ABSTIME)` |
| `xplat_user_dir` | known folders (Roaming/Local AppData), then `%APPDATA%`/`%LOCALAPPDATA%` | XDG variables when absolute, else `~/.config`, `~/.local/share`, `~/.cache`, `~/.local/state` |

macOS builds too (`_NSGetExecutablePath`, `nanosleep`), untested.

## Adopting it in a game

```cmake
# XPLAT_DIR: the project's xplat/ folder. A game configured from a copy of its folder (the setup tool's work
# folder) is told where it is; built by hand inside the project, it is two folders up.
if(NOT XPLAT_DIR)
    get_filename_component(XPLAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../xplat" ABSOLUTE)
endif()
include("${XPLAT_DIR}/cmake/Xplat.cmake")
xplat_add_library()
target_link_libraries(<game> PRIVATE xplat::xplat)
xplat_windows_app(<game> GUI WMAIN)   # the shared manifest: UTF-8 code page + long paths
xplat_find_ngl_vk(<PREFIX> TARGET <game> DEFINE <GAME>_HAS_NGL_VK)   # if the game uses the Vulkan renderer
```

C++ code includes `xplat/xplat.hpp` (C++17 or C++20). On Windows, call `xplat_utf8_args(&argc, &argv)` at the
top of `main` (with the SDK's `wWinMain` the arguments are already wide; convert them with `Utf8FromPath`).

Adopters (each its own candidate, each landed by its owner):

| game | what changes | owner |
|---|---|---|
| Call of Duty 3 | `util.h`'s path helpers become thin wrappers (same results); the manifest and the ngl_vk search move to the modules; nothing in the game's file locations changes | rexglue-native |
| Xbox 360 ports | the ngl_vk search (through `XPLAT_DIR`, not `../../ngl_vk`), the manifest, the guest-address helper | ports |
| Big Red One | the manifest, the monotonic clock in ngl_vk's pacing | waw-decomp/bro1 |
| Finest Hour | `argv` and 8 `fopen` calls are narrow (names outside the ANSI code page fail): `xplat_utf8_args`, `xplat_fopen`; 4 timers read the wall clock: `xplat_monotonic_ns` | waw-decomp/fh1 |

## Windows toolchains

Both are first class, and the modules choose by themselves:

* **llvm-mingw** (the Windows build made on Linux): the manifest goes in as a resource (`win/xplat_manifest.rc`),
  byte for byte `win/xplat.manifest`.
* **MSVC ABI** (the build on a Windows PC: clang for `x86_64-pc-windows-msvc` with lld-link, the MSVC runtime and the
  Windows SDK; or MSVC itself): the manifest is a source, CMake passes `/MANIFESTINPUT:`, and the linker merges it
  into the one it makes (one RT_MANIFEST with `asInvoker`, the UTF-8 code page and `longPathAware`). A resource
  script there would be a second manifest #1, which lld-link refuses: *duplicate resource: type MANIFEST (ID 24)/name
  ID 1*. The merge needs an lld-link built with libxml2 (the LLVM Windows installer's, a Linux distribution's lld) or
  `mt.exe` on PATH; llvm-mingw's lld-link has neither and says *unable to find mt.exe in PATH*. Data exported from a
  DLL needs `__declspec(dllimport)` on this ABI (MinGW imports it by itself; lld-link says *undefined symbol*).

## Building and testing xplat itself

```sh
cmake -S xplat -B build -DXPLAT_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
```

For Windows, configure the same with a toolchain file for llvm-mingw or for clang's MSVC target, and run the tests on
Windows or under Wine. They cover names outside ASCII (a surrogate pair too), paths over 300 characters, the `\\?\`
and UNC forms, rename-replace, directory listing, the environment, the program's own path, the user folders, the
clock, and an argument outside ASCII on the command line. The sources build without a warning under
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion` with gcc, clang, llvm-mingw and clang for the MSVC target.
