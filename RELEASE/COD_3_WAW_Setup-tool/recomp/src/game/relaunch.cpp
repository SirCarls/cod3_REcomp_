// cod3 - relaunching for the game's "reboot" to the main menu (see relaunch.h).

#include "game/relaunch.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "util.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW (shell32)
#else
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

extern char** environ;
#endif

REXCVAR_DEFINE_STRING(cod3_launch_data, "", "cod3/Internal",
                      "Set by the program itself when the game reboots to its main menu: the "
                      "file with the game's launch data")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// [claude-9b-think] 2026-10-28 v2 UNTESTED: see relaunch.h StartedByLevelGuard.
REXCVAR_DEFINE_BOOL(cod3_relaunch_by_level_guard, false, "cod3/Internal",
                    "Set by the program itself when the console's level-command guard restarted it: the "
                    "guard doesn't restart this instance again in its first minute")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace cod3 {

namespace {

constexpr const char* kLaunchDataArg = "--cod3_launch_data=";
constexpr const char* kLaunchDataFile = "launch_data.bin";
// The Xbox 360's limit for launch data (XLAUNCH_DATA is 3 KB; the game stores
// its 664-byte control settings). Anything larger is not the game's and is
// not handed to it.
constexpr uintmax_t kMaxLaunchDataBytes = 3072;

rex::kernel::xam::XamModule::LoaderData* LoaderData() {
  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state) return nullptr;
  auto xam = kernel_state->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  return xam ? &xam->loader_data() : nullptr;
}

bool EndsWith(const std::string& path, const std::string& name) {
  return path.size() >= name.size() && path.compare(path.size() - name.size(), name.size(), name) == 0;
}

// The program for the executable the game asked for ("d:\default.xex",
// "d:\codmp_xenonf.xex"; the names are the disc's), or none.
std::optional<Program> ProgramForPath(std::string path) {
  for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (EndsWith(path, "default.xex")) return Program::kCampaign;
  if (EndsWith(path, "codmp_xenonf.xex")) return Program::kMultiplayer;
  return std::nullopt;
}

// The executable the game asked for, for the log: its file name only, with
// any byte that isn't printable shown as \xNN. The SDK's launch path can
// start with stray bytes (XamLoaderLaunchTitle joins a bare "default.xex" to
// the running program's folder through a view of a freed string).
std::string LaunchName(const std::string& path) {
  const size_t cut = path.find_last_of("\\/:");
  std::string out;
  for (const char c : cut == std::string::npos ? path : path.substr(cut + 1)) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u < 0x7F) {
      out.push_back(c);
    } else {
      static constexpr char kHex[] = "0123456789ABCDEF";
      out += "\\x";
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 15]);
    }
  }
  return out;
}

// AppRun (c3w/pipeline.py) starts usr/bin/$COD3_PROGRAM; any other value
// starts the campaign.
constexpr const char* kProgramEnv = "COD3_PROGRAM";

// The program this file is compiled into (recomp/mp/CMakeLists.txt defines
// COD3_MP for cod3mp). It restarts itself from its own path, whatever the
// file is called; the other program is its sibling by name.
#if defined(COD3_MP)
constexpr Program kThisProgram = Program::kMultiplayer;
#else
constexpr Program kThisProgram = Program::kCampaign;
#endif

bool IsLaunchDataArg(const std::string& arg) { return arg.rfind(kLaunchDataArg, 0) == 0; }

// Test switches that act once per start (a console script, scripted input, a
// battle action demo): not handed to the next instance, or quitting to the
// menu would run "spmap ..." again, and a "reboot" in it would never end.
bool IsOneOffArg(const std::string& arg) {
  for (const char* prefix :
       {"--cod3_autoexec", "--cod3_test_", "--cod3_qte_demo", "--cod3_relaunch_by_level_guard"})
    if (arg.rfind(prefix, 0) == 0) return true;
  return false;
}

// The window now (NoteWindowState); width 0 until the UI has run.
std::atomic<bool> g_fullscreen{false};
std::atomic<bool> g_has_focus{true};
std::atomic<uint32_t> g_window_width{0}, g_window_height{0};

// Arguments the new instance gets instead of the old ones about the window.
bool IsWindowArg(const std::string& arg) {
  for (const char* name :
       {"--fullscreen", "--window_width", "--window_height", "--start_in_background"}) {
    const size_t n = std::char_traits<char>::length(name);
    if (arg.compare(0, n, name) == 0 && (arg.size() == n || arg[n] == '=')) return true;
  }
  return false;
}
std::vector<std::string> WindowArgs() {
  std::vector<std::string> out;
  if (!g_window_width.load(std::memory_order_relaxed)) return out;  // never shown: keep the old ones
  const bool fullscreen = g_fullscreen.load(std::memory_order_relaxed);
  out.push_back(std::string("--fullscreen=") + (fullscreen ? "true" : "false"));
  if (!fullscreen) {
    out.push_back("--window_width=" + std::to_string(g_window_width.load(std::memory_order_relaxed)));
    out.push_back("--window_height=" + std::to_string(g_window_height.load(std::memory_order_relaxed)));
  }
  // SDK patch 0063 (an SDK without it would refuse the whole command line).
  if (rex::cvar::GetFlagInfo("start_in_background"))
    out.push_back(std::string("--start_in_background=") +
                  (g_has_focus.load(std::memory_order_relaxed) ? "false" : "true"));
  return out;
}

// [claude-9b-think] 2026-10-28 v2 UNTESTED: SetStartupCommand's line, for the next instance only.
std::mutex g_startup_mutex;
std::string g_startup_command;

// The startup line as the new instance's --cod3_autoexec, and the mark that the guard started it (the old command
// line's one-off arguments, an old --cod3_autoexec and an old mark among them, are left out before these are
// added).
std::vector<std::string> StartupArgs() {
  std::lock_guard<std::mutex> lock(g_startup_mutex);
  std::vector<std::string> out;
  if (g_startup_command.empty()) return out;
  out.push_back("--cod3_autoexec=" + g_startup_command);
  out.push_back("--cod3_relaunch_by_level_guard=true");
  return out;
}

#if defined(_WIN32)
// Quotes one argument the way CommandLineToArgvW reads it back.
std::wstring QuoteArg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
    else out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

// The program's executable: this one, or the other next to it.
std::filesystem::path ProgramPath(Program program) {
  wchar_t self[32768];
  const DWORD len = GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  if (!len || len >= std::size(self)) return {};
  if (program == kThisProgram) return std::filesystem::path(self);
  return std::filesystem::path(self).parent_path() /
         (std::filesystem::path(ProgramName(program)).wstring() + L".exe");
}

bool Spawn(Program program, const std::filesystem::path& data_file) {
  const std::wstring exe = ProgramPath(program).wstring();
  if (exe.empty()) return false;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::wstring cmd = QuoteArg(exe);
  const std::wstring skip = std::filesystem::path(kLaunchDataArg).wstring();
  const bool window_known = !WindowArgs().empty();
  for (int i = 1; argv && i < argc; ++i) {
    if (std::wstring(argv[i]).rfind(skip, 0) == 0) continue;
    if (window_known && IsWindowArg(std::filesystem::path(argv[i]).string())) continue;
    if (IsOneOffArg(std::filesystem::path(argv[i]).string())) continue;
    cmd += L" " + QuoteArg(argv[i]);
  }
  if (argv) LocalFree(argv);
  for (const std::string& arg : WindowArgs()) cmd += L" " + std::filesystem::path(arg).wstring();
  // [claude-9b-think] SetStartupCommand's line (it has a space: quoted) and the guard's mark.
  for (const std::string& arg : StartupArgs())
    cmd += L" " + QuoteArg(std::filesystem::path(arg).wstring());
  cmd += L" " + QuoteArg(skip + data_file.wstring());
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}
#else
// The program's executable: this one, or the other next to it (inside an
// AppImage, in its mount's usr/bin).
std::filesystem::path ProgramPath(Program program) {
  std::error_code ec;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || self.empty()) return {};
  if (program == kThisProgram) return self;
  return self.parent_path() / ProgramName(program);
}

bool Spawn(Program program_id, const std::filesystem::path& data_file) {
  // The program: the AppImage file itself when running from one (its mount
  // goes away when this process ends; its AppRun starts the program named by
  // COD3_PROGRAM), else the program's executable next to this one.
  // APPIMAGE only counts when this executable is inside that AppImage's
  // mount (APPDIR): a program started from a terminal of another AppImage
  // inherits that one's APPIMAGE, and must not start it.
  std::error_code ec;
  const std::string self = std::filesystem::read_symlink("/proc/self/exe", ec).string();
  if (ec || self.empty()) return false;
  std::string program = ProgramPath(program_id).string();
  const char* appimage = std::getenv("APPIMAGE");
  const char* appdir = std::getenv("APPDIR");
  if (appimage && *appimage && appdir && *appdir) {
    std::string mount = appdir;
    if (mount.back() != '/') mount += '/';
    if (self.rfind(mount, 0) == 0) program = appimage;
  }
  // The same arguments (from /proc/self/cmdline), without an old launch data one.
  std::vector<std::string> args{program};
  {
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t start = 0, index = 0;
    while (start < all.size()) {
      size_t end = all.find('\0', start);
      if (end == std::string::npos) end = all.size();
      std::string arg = all.substr(start, end - start);
      const bool replaced = IsWindowArg(arg) && !WindowArgs().empty();
      if (index++ > 0 && !IsLaunchDataArg(arg) && !replaced && !IsOneOffArg(arg))
        args.push_back(std::move(arg));
      start = end + 1;
    }
  }
  for (std::string& arg : WindowArgs()) args.push_back(std::move(arg));
  for (std::string& arg : StartupArgs()) args.push_back(std::move(arg));  // [claude-9b-think]
  args.push_back(kLaunchDataArg + data_file.string());
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  // The environment, with COD3_PROGRAM naming the program for an AppImage's
  // AppRun (made here: the child of a multi-threaded process must not
  // allocate).
  const std::string program_env = std::string(kProgramEnv) + "=" + ProgramName(program_id);
  std::vector<std::string> env_strings{program_env};
  for (char** e = environ; e && *e; ++e)
    if (std::strncmp(*e, kProgramEnv, std::strlen(kProgramEnv)) != 0 ||
        (*e)[std::strlen(kProgramEnv)] != '=')
      env_strings.emplace_back(*e);
  std::vector<char*> envp;
  for (auto& e : env_strings) envp.push_back(e.data());
  envp.push_back(nullptr);

  // Everything is prepared before fork(): the child only closes inherited
  // descriptors (the guest memory, the GPU, audio ...) and replaces itself.
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
    {
      const long max_fd = sysconf(_SC_OPEN_MAX);
      for (long fd = 3; fd < (max_fd > 0 ? max_fd : 4096); ++fd) close(int(fd));
    }
    execve(argv[0], argv.data(), envp.data());
    _exit(127);
  }
  return true;
}
#endif

}  // namespace


// [claude-9b-think] 2026-10-28 v2 UNTESTED: see relaunch.h.
void SetStartupCommand(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_startup_mutex);
  g_startup_command = line;
}

bool StartedByLevelGuard() { return REXCVAR_GET(cod3_relaunch_by_level_guard); }

void UseStartInBackgroundAsLaunchOptionOnly() {
  if (!rex::cvar::GetFlagInfo("start_in_background")) return;  // an SDK without patch 0063
  const auto source = rex::cvar::GetFlagSource("start_in_background");
  if (source == rex::cvar::Source::kDefault || source == rex::cvar::Source::kCommandLine ||
      source == rex::cvar::Source::kEnvironment)
    return;
  REXLOG_INFO("cod3: start_in_background from the settings not used (a launch option: "
              "--start_in_background=true)");
  rex::cvar::ResetToDefault("start_in_background");
}

void NoteWindowState(bool fullscreen, uint32_t logical_width, uint32_t logical_height,
                     bool has_focus) {
  g_fullscreen.store(fullscreen, std::memory_order_relaxed);
  g_has_focus.store(has_focus, std::memory_order_relaxed);
  g_window_width.store(logical_width, std::memory_order_relaxed);
  g_window_height.store(logical_height, std::memory_order_relaxed);
}

const char* ProgramName(Program program) {
  return program == Program::kMultiplayer ? "cod3mp" : "cod3";
}

bool ProgramInstalled(Program program) {
  const auto path = ProgramPath(program);
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

bool RelaunchRequested() {
  const auto* data = LoaderData();
  if (!data || data->launch_path.empty()) return false;
  const auto program = ProgramForPath(data->launch_path);
  if (!program) {
    REXLOG_WARN("cod3: the game asked to launch {}, which this port doesn't have",
                LaunchName(data->launch_path));
    return false;
  }
  if (!ProgramInstalled(*program)) {
    REXLOG_WARN("cod3: the game asked to launch {}; its program ({}) isn't next to this one",
                LaunchName(data->launch_path), ProgramName(*program));
    return false;
  }
  return true;
}

bool Relaunch(const std::filesystem::path& userdata_dir) {
  const auto* data = LoaderData();
  if (!data) return false;
  const auto program = ProgramForPath(data->launch_path);
  if (!program) return false;
  std::error_code ec;
  std::filesystem::create_directories(userdata_dir, ec);
  const auto file = userdata_dir / kLaunchDataFile;
  if (data->launch_data.size() > kMaxLaunchDataBytes) {
    REXLOG_ERROR("cod3: the game's launch data is {} bytes, more than the {} the console allows; "
                 "not relaunching with it",
                 data->launch_data.size(), kMaxLaunchDataBytes);
    return false;
  }
  // [claude-9b-think] 2026-10-28 v3 UNTESTED (claude-9b-test g2-2: after the guard's restart the new instance loaded
  // saint_lo by itself 0.25 s after its start, before its autoexec, then refused its own carried 'spmap blkbrn' as a
  // second level). The game's reboot hands the next instance its launch data, and a game started with launch data goes
  // straight back into the level it names (a cold start, g2-1, loads none). For the guard's restart the carried command
  // is the level to load, so the new instance starts cold: no launch data (its control settings come from the profile,
  // as at any start). The game's own reboots (quitting to the menu) keep theirs.
  bool guard_restart = false;
  {
    std::lock_guard<std::mutex> lock(g_startup_mutex);
    guard_restart = !g_startup_command.empty();
  }
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!guard_restart && data->launch_data_present && !data->launch_data.empty())
      out.write(reinterpret_cast<const char*>(data->launch_data.data()),
                std::streamsize(data->launch_data.size()));
    if (!out) {
      REXLOG_ERROR("cod3: couldn't write {} for the relaunch", PathToUtf8(file));
      return false;
    }
  }
  if (!Spawn(*program, file)) {
    REXLOG_ERROR("cod3: couldn't start {} for the game's launch of {}", ProgramName(*program),
                 LaunchName(data->launch_path));
    return false;
  }
  if (guard_restart)
    REXLOG_INFO("cod3: the game launched {}; started {} for the level command, cold (its {} bytes of launch data "
                "left out, so it doesn't load this level again first)",
                LaunchName(data->launch_path), ProgramName(*program), data->launch_data.size());
  else
    REXLOG_INFO("cod3: the game launched {}; started {} ({} bytes of launch data)",
                LaunchName(data->launch_path), ProgramName(*program), data->launch_data.size());
  return true;
}

void RestoreLaunchData(const std::filesystem::path& userdata_dir) {
  const std::string path = REXCVAR_GET(cod3_launch_data);
  if (path.empty()) return;
  // Only Relaunch's command line hands launch data over. A value saved in
  // cod3.toml by an older build would give a fresh start whatever file a
  // failed relaunch left behind (old control settings).
  if (rex::cvar::GetFlagSource("cod3_launch_data") != rex::cvar::Source::kCommandLine) {
    rex::cvar::ResetToDefault("cod3_launch_data");
    return;
  }
  const std::filesystem::path file = Utf8ToPath(path);
  // Only the port's own file: userdata/launch_data.bin, written by Relaunch
  // above (the argument comes from the command line, which anything that
  // starts the program could set), and no larger than the console allows.
  std::error_code ec;
  const auto expected = std::filesystem::weakly_canonical(userdata_dir / kLaunchDataFile, ec);
  const auto given = std::filesystem::weakly_canonical(file, ec);
  if (ec || expected.empty() || given != expected) {
    REXLOG_WARN("cod3: cod3_launch_data doesn't name userdata/{}; ignored", kLaunchDataFile);
    return;
  }
  const uintmax_t size = std::filesystem::file_size(file, ec);
  if (ec || size > kMaxLaunchDataBytes) {
    REXLOG_WARN("cod3: {} is {} bytes (the limit is {}); ignored and deleted", PathToUtf8(file),
                ec ? 0 : size, kMaxLaunchDataBytes);
    std::filesystem::remove(file, ec);
    return;
  }
  std::ifstream in(file, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::filesystem::remove(file, ec);
  if (bytes.size() > kMaxLaunchDataBytes) return;  // grew between the check and the read
  auto* data = LoaderData();
  if (!data) return;
  data->launch_data = std::move(bytes);
  data->launch_data_present = !data->launch_data.empty();
  REXLOG_INFO("cod3: started by the game's launch of this program ({} bytes of launch data)",
              data->launch_data.size());
}

}  // namespace cod3
