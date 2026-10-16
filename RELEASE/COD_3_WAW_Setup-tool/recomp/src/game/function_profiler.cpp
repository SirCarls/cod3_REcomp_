// cod3 - the in-engine function profiler (see function_profiler.h).

#include "game/function_profiler.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "game/diagnostics.h"
#include "game/thread_profiler.h"

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <cxxabi.h>
#include <elf.h>
#include <link.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

// The recompiled code's guest-to-host function table (generated/default/
// cod3_init.cpp): every game function's Xbox 360 address and the host
// function it became, ending with {0, nullptr}.
extern PPCFuncMapping PPCFuncMappings[];

namespace cod3 {

namespace {

using Clock = std::chrono::steady_clock;

// Samples are instruction addresses, written by the signal handler (Linux)
// or the sampling thread (Windows). 120 s at 1 kHz fits.
constexpr uint32_t kMaxSamples = 128 * 1024;
std::unique_ptr<std::atomic<uintptr_t>[]> g_samples;
std::atomic<uint32_t> g_sample_count{0};
std::atomic<bool> g_busy{false};

void RecordSample(uintptr_t pc) {
  const uint32_t i = g_sample_count.fetch_add(1, std::memory_order_relaxed);
  if (i < kMaxSamples) g_samples[i].store(pc, std::memory_order_relaxed);
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The thread the user asked for, from the thread profiler's last table.
bool FindThread(const std::string& wanted, ThreadRow* found) {
  const ThreadReport report = ThreadProfiler::Get().Report();
  const std::string w = Lower(wanted);
  const char* role = w == "main" || w == "game" ? "game main thread"
                     : w == "gpu"               ? "GPU emulation"
                     : w == "window" || w == "ui" || w == "present" ? "window, input, presenting"
                                                                     : nullptr;
  for (const ThreadRow& row : report.rows) {
    if (role ? row.role == role : Lower(row.name).rfind(w, 0) == 0) {
      // Several threads can share a name (the four job workers): take the
      // busiest, which comes first.
      *found = row;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- symbols --

struct Symbol {
  uintptr_t address = 0;
  uintptr_t size = 0;
  std::string name;
};

struct Module {
  std::string path;   // file
  std::string label;  // what the report calls it
  uintptr_t base = 0;
  std::vector<std::pair<uintptr_t, uintptr_t>> code;  // [begin, end) ranges
  std::vector<Symbol> symbols;                        // sorted by address
  bool symbols_read = false;
};

std::string Demangle(const char* name) {
#if defined(__linux__)
  int status = 0;
  char* out = abi::__cxa_demangle(name, nullptr, nullptr, &status);
  if (status == 0 && out) {
    std::string s(out);
    std::free(out);
    // Keep the function's name, not its parameter list.
    const size_t paren = s.find('(');
    if (paren != std::string::npos && paren > 0) s.resize(paren);
    if (s.size() > 90) s = s.substr(0, 87) + "...";
    return s;
  }
#endif
  return name;
}

std::string ModuleLabel(const std::string& path) {
  const std::string file = std::filesystem::path(path).filename().string();
  if (file.rfind("librexruntime", 0) == 0 || file.rfind("rexruntime", 0) == 0)
    return "ReXGlue runtime (" + file + ")";
  if (file.rfind("libcod3_", 0) == 0 || file.rfind("cod3_", 0) == 0)
    return "level code (" + file + ")";
  if (file.rfind("libc.so", 0) == 0 || file.rfind("libm.so", 0) == 0 ||
      file.rfind("libstdc++", 0) == 0 || file.rfind("libc++", 0) == 0 || file == "ucrtbase.dll" ||
      file == "msvcrt.dll")
    return "C / C++ library (" + file + ")";
  if (file.find("vulkan") != std::string::npos || file.find("nvidia") != std::string::npos ||
      file.find("radeon") != std::string::npos || file.find("lvp") != std::string::npos ||
      file.find("gallium") != std::string::npos || file.find("amdvlk") != std::string::npos ||
      file.find("d3d12") != std::string::npos || file.find("dxgi") != std::string::npos ||
      file.find("nvoglv") != std::string::npos || file.find("amdxc") != std::string::npos)
    return "graphics driver (" + file + ")";
  return file;
}

#if defined(__linux__)

std::vector<Module> LoadedModules() {
  std::vector<Module> modules;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) -> int {
        auto& out = *static_cast<std::vector<Module>*>(data);
        Module m;
        m.path = info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : "/proc/self/exe";
        // The kernel's vDSO (clock_gettime and friends without a system
        // call) has no file to read symbols from: named, but not by function.
        const bool vdso = m.path.find("linux-vdso") != std::string::npos;
        m.base = info->dlpi_addr;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const ElfW(Phdr)& ph = info->dlpi_phdr[i];
          if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X))
            m.code.push_back({m.base + ph.p_vaddr, m.base + ph.p_vaddr + ph.p_memsz});
        }
        m.label = m.path == "/proc/self/exe" ? "game code (cod3)" : ModuleLabel(m.path);
        if (vdso) {
          m.label = "kernel vDSO (clock reads)";
          m.symbols_read = true;
        }
        if (!m.code.empty()) out.push_back(std::move(m));
        return 0;
      },
      &modules);
  return modules;
}

// Function symbols from the file's .symtab (or .dynsym when stripped).
void ReadSymbols(Module& m) {
  m.symbols_read = true;
  std::ifstream f(m.path, std::ios::binary);
  if (!f) return;
  ElfW(Ehdr) eh{};
  if (!f.read(reinterpret_cast<char*>(&eh), sizeof(eh)) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != (sizeof(void*) == 8 ? ELFCLASS64 : ELFCLASS32) ||
      eh.e_shentsize != sizeof(ElfW(Shdr)))
    return;
  std::vector<ElfW(Shdr)> sections(eh.e_shnum);
  f.seekg(std::streamoff(eh.e_shoff));
  if (!f.read(reinterpret_cast<char*>(sections.data()), std::streamsize(sections.size() * sizeof(ElfW(Shdr)))))
    return;
  auto read_table = [&](uint32_t type) -> bool {
    for (const ElfW(Shdr)& sh : sections) {
      if (sh.sh_type != type || sh.sh_link >= sections.size() || sh.sh_entsize != sizeof(ElfW(Sym))) continue;
      const ElfW(Shdr)& strs = sections[sh.sh_link];
      std::vector<char> names(strs.sh_size + 1, '\0');
      std::vector<ElfW(Sym)> syms(sh.sh_size / sizeof(ElfW(Sym)));
      f.clear();
      f.seekg(std::streamoff(strs.sh_offset));
      if (!f.read(names.data(), std::streamsize(strs.sh_size))) return false;
      f.seekg(std::streamoff(sh.sh_offset));
      if (!f.read(reinterpret_cast<char*>(syms.data()), std::streamsize(syms.size() * sizeof(ElfW(Sym)))))
        return false;
      for (const ElfW(Sym)& s : syms) {
        if (ELF64_ST_TYPE(s.st_info) != STT_FUNC || s.st_value == 0 || s.st_name >= strs.sh_size) continue;
        m.symbols.push_back({m.base + s.st_value, s.st_size, Demangle(&names[s.st_name])});
      }
      return !m.symbols.empty();
    }
    return false;
  };
  if (!read_table(SHT_SYMTAB)) read_table(SHT_DYNSYM);
  std::sort(m.symbols.begin(), m.symbols.end(),
            [](const Symbol& a, const Symbol& b) { return a.address < b.address; });
}

#elif defined(_WIN32)

std::vector<Module> LoadedModules() { return {}; }  // filled per address below
void ReadSymbols(Module& m) { m.symbols_read = true; }

#endif

// The recompiled functions by host address: guest address and the host
// function's start, for naming code without symbols (Windows) and adding the
// Xbox 360 address everywhere.
struct GuestTable {
  std::vector<std::pair<uintptr_t, uint32_t>> by_host;  // sorted
  GuestTable() {
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
      by_host.push_back({reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest)});
    std::sort(by_host.begin(), by_host.end());
  }
  // The game function a host address belongs to: the nearest start below it,
  // when it's no further than a large function would be.
  bool Find(uintptr_t pc, uintptr_t* start, uint32_t* guest) const {
    auto it = std::upper_bound(by_host.begin(), by_host.end(), std::make_pair(pc, UINT32_MAX));
    if (it == by_host.begin()) return false;
    --it;
    if (pc - it->first > 512 * 1024) return false;
    *start = it->first;
    *guest = it->second;
    return true;
  }
  bool Exact(uintptr_t host, uint32_t* guest) const {
    auto it = std::lower_bound(by_host.begin(), by_host.end(), std::make_pair(host, uint32_t(0)));
    if (it == by_host.end() || it->first != host) return false;
    *guest = it->second;
    return true;
  }
};

// One place in the report: a function (or a module when nothing names it).
struct Place {
  std::string name;
  std::string module;
  uint32_t guest = 0;
  uint64_t samples = 0;
};

// ----------------------------------------------------------------- report --

std::filesystem::path WriteProfile(const ThreadRow& thread, double seconds, double cpu_share,
                                   const std::vector<uintptr_t>& pcs) {
  std::vector<Module> modules = LoadedModules();
  const GuestTable guest_table;

  std::map<std::string, Place> places;      // by name + module
  std::map<std::string, uint64_t> by_module;
  for (uintptr_t pc : pcs) {
    Module* mod = nullptr;
    for (Module& m : modules)
      for (const auto& r : m.code)
        if (pc >= r.first && pc < r.second) mod = &m;
    Place p;
    if (mod) {
      if (!mod->symbols_read) ReadSymbols(*mod);
      p.module = mod->label;
      auto it = std::upper_bound(mod->symbols.begin(), mod->symbols.end(), pc,
                                 [](uintptr_t a, const Symbol& s) { return a < s.address; });
      if (it != mod->symbols.begin()) {
        --it;
        if (it->size == 0 || pc < it->address + it->size) {
          p.name = it->name;
          guest_table.Exact(it->address, &p.guest);
        }
      }
    }
    if (p.name.empty()) {
      uintptr_t start = 0;
      uint32_t guest = 0;
      if (guest_table.Find(pc, &start, &guest) && (!mod || mod->label == "game code (cod3)")) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "sub_%08X", guest);
        p.name = buf;
        p.guest = guest;
        if (p.module.empty()) p.module = "game code (cod3)";
      }
    }
#if defined(_WIN32)
    if (p.module.empty()) {
      HMODULE h = nullptr;
      wchar_t path[MAX_PATH];
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(pc), &h) &&
          GetModuleFileNameW(h, path, MAX_PATH)) {
        p.module = ModuleLabel(std::filesystem::path(path).string());
        if (p.name.empty()) {
          char buf[48];
          std::snprintf(buf, sizeof(buf), "+0x%" PRIxPTR, pc - reinterpret_cast<uintptr_t>(h));
          p.name = buf;
        }
      }
    }
#endif
    if (p.module.empty()) p.module = "unknown";
    if (p.name.empty()) p.name = "(no symbol)";
    by_module[p.module]++;
    Place& slot = places[p.module + "\n" + p.name];
    if (slot.samples == 0) slot = p;
    slot.samples++;
  }

  std::vector<Place> ranked;
  for (auto& [key, place] : places) ranked.push_back(place);
  std::sort(ranked.begin(), ranked.end(), [](const Place& a, const Place& b) { return a.samples > b.samples; });
  std::vector<std::pair<std::string, uint64_t>> modules_ranked(by_module.begin(), by_module.end());
  std::sort(modules_ranked.begin(), modules_ranked.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::string tag;
  for (char c : thread.role.empty() ? thread.name : thread.role)
    tag += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
  while (tag.find("--") != std::string::npos) tag.erase(tag.find("--"), 1);
  if (tag.size() > 24) tag.resize(24);
  char name[96];
  std::snprintf(name, sizeof(name), "profile-%s-%04d%02d%02d-%02d%02d%02d.txt", tag.c_str(), tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  std::error_code ec;
  const auto dir = diag::LogsDir();
  std::filesystem::create_directories(dir, ec);
  const auto path = dir / name;
  std::ofstream out(path, std::ios::trunc);
  if (!out) return {};

  const double total = double(std::max<size_t>(pcs.size(), 1));
  char line[512];
  out << "Call of Duty 3 (recomp) function profile\n";
  std::snprintf(line, sizeof(line), "Thread: %s (%s), OS id %u\n", thread.name.c_str(),
                thread.role.empty() ? "no known role" : thread.role.c_str(), thread.id);
  out << line;
  std::snprintf(line, sizeof(line),
                "Captured %.1f s: %zu samples, taken while the thread ran; it was busy %.0f%% of "
                "the time (its CPU time).\n",
                seconds, pcs.size(), cpu_share * 100.0);
  out << line;
  out << "Self time: the function the thread was running when sampled (not counting what it\n"
         "called). Game functions carry their Xbox 360 address.\n\n";

  out << "== Where the time went, by module ==\n";
  for (const auto& [module, n] : modules_ranked) {
    std::snprintf(line, sizeof(line), "%6.1f%%  %s\n", 100.0 * double(n) / total, module.c_str());
    out << line;
  }
  out << "\n== Hottest functions (self time) ==\n";
  size_t shown = 0;
  for (const Place& p : ranked) {
    if (shown++ == 60) break;
    char guest[24] = "";
    if (p.guest) std::snprintf(guest, sizeof(guest), "  [0x%08X]", p.guest);
    std::snprintf(line, sizeof(line), "%6.1f%%  %-48s %s%s\n", 100.0 * double(p.samples) / total,
                  p.name.c_str(), p.module.c_str(), guest);
    out << line;
  }

  // One line for the log too: the three hottest.
  std::string top;
  for (size_t i = 0; i < ranked.size() && i < 3; ++i) {
    char part[160];
    std::snprintf(part, sizeof(part), "%s%s %.1f%%", i ? ", " : "", ranked[i].name.c_str(),
                  100.0 * double(ranked[i].samples) / total);
    top += part;
  }
  REXLOG_INFO("cod3: function profile of {} ({}), {} samples: {}; written to {}", thread.name, thread.role,
              pcs.size(), top, path.filename().string());
  return path;
}

// ---------------------------------------------------------------- capture --

#if defined(__linux__)

// The kernel's id for a thread's CPU-time clock (make_thread_cpuclock(tid,
// CPUCLOCK_SCHED) in the kernel's include/linux/posix-timers.h, what glibc's
// pthread_getcpuclockid builds for its own threads).
clockid_t ThreadCpuClock(pid_t tid) { return clockid_t((~unsigned(tid) << 3) | 6); }

void OnProfSignal(int, siginfo_t*, void* context) {
  const int saved_errno = errno;
  const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
  RecordSample(uintptr_t(uc->uc_mcontext.gregs[REG_RIP]));
#elif defined(__aarch64__)
  RecordSample(uintptr_t(uc->uc_mcontext.pc));
#endif
  errno = saved_errno;
}

double CpuSeconds(pid_t tid) {
  timespec ts{};
  if (clock_gettime(ThreadCpuClock(tid), &ts) != 0) return 0.0;
  return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

// Samples `tid` for `seconds`; returns false when the timer can't be set up.
// `cpu_seconds`: the CPU time the thread used meanwhile. CPU-time timers are
// only as fine as the kernel's software clock (time(7): HZ is 100, 250, 300
// or 1000 by kernel), so a thread gets at most one sample per tick: the share
// of the time it was busy comes from its CPU clock, not from the samples.
bool Capture(uint32_t tid, int seconds, double* cpu_seconds) {
  struct sigaction action{}, previous{};
  action.sa_sigaction = OnProfSignal;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPROF, &action, &previous) != 0) return false;

  sigevent event{};
  event.sigev_notify = SIGEV_THREAD_ID;
  event.sigev_signo = SIGPROF;
#if defined(sigev_notify_thread_id)
  event.sigev_notify_thread_id = pid_t(tid);
#else
  event._sigev_un._tid = pid_t(tid);
#endif
  // Raw system calls: the C library's timer_create needs librt on older
  // systems, and the kernel's timer id is a plain int.
  int timer = -1;
  if (syscall(SYS_timer_create, ThreadCpuClock(pid_t(tid)), &event, &timer) != 0) {
    sigaction(SIGPROF, &previous, nullptr);
    return false;
  }
  itimerspec every_ms{};
  every_ms.it_interval.tv_nsec = 1000000;
  every_ms.it_value.tv_nsec = 1000000;
  const double cpu_start = CpuSeconds(pid_t(tid));
  syscall(SYS_timer_settime, timer, 0, &every_ms, nullptr);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  syscall(SYS_timer_delete, timer);
  *cpu_seconds = CpuSeconds(pid_t(tid)) - cpu_start;
  // A signal already on its way still finds the handler.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  sigaction(SIGPROF, &previous, nullptr);
  return true;
}

#elif defined(_WIN32)

double CpuSeconds(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0.0;
  auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return double(ticks(kernel) + ticks(user)) / 1e7;  // 100 ns units
}

bool Capture(uint32_t tid, int seconds, double* cpu_seconds) {
  HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!h) return false;
  const double cpu_start = CpuSeconds(h);
  const auto end = Clock::now() + std::chrono::seconds(seconds);
  ULONG64 last_cycles = 0;
  QueryThreadCycleTime(h, &last_cycles);
  while (Clock::now() < end) {
    Sleep(1);
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(h, &cycles) || cycles == last_cycles) continue;  // it didn't run
    last_cycles = cycles;
    if (SuspendThread(h) == DWORD(-1)) continue;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(h, &ctx)) {
#if defined(_M_X64) || defined(__x86_64__)
      RecordSample(uintptr_t(ctx.Rip));
#elif defined(_M_ARM64) || defined(__aarch64__)
      RecordSample(uintptr_t(ctx.Pc));
#endif
    }
    ResumeThread(h);
  }
  *cpu_seconds = CpuSeconds(h) - cpu_start;
  CloseHandle(h);
  return true;
}

#else

bool Capture(uint32_t, int, double*) { return false; }

#endif

}  // namespace

bool FunctionProfiler::Busy() { return g_busy.load(); }

std::string FunctionProfiler::Start(const std::string& thread, int seconds, int delay_seconds) {
#if !defined(__linux__) && !defined(_WIN32)
  return "cod3_profile works on Linux and Windows only";
#else
  seconds = std::clamp(seconds, 1, 120);
  delay_seconds = std::clamp(delay_seconds, 0, 30);
  const std::string wanted = thread.empty() ? "main" : thread;
  // The thread is looked up when the capture starts (after the delay), so a
  // capture asked for during a load still finds it; checked now as well when
  // there's no delay to wait for.
  ThreadRow row;
  const bool known_now = FindThread(wanted, &row);
  if (!known_now && delay_seconds == 0)
    return "cod3_profile: no thread \"" + wanted +
           "\" in the thread table yet (main, gpu, window, or the start of a thread name)";
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) return "cod3_profile: a capture is already running";
  if (!g_samples) g_samples.reset(new std::atomic<uintptr_t>[kMaxSamples]);
  g_sample_count.store(0);

  std::thread([wanted, seconds, delay_seconds] {
    std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
    ThreadRow row;
    if (!FindThread(wanted, &row)) {
      REXLOG_WARN("cod3: function profile: no thread \"{}\" (main, gpu, window, or the start of a "
                  "thread name)", wanted);
      g_busy.store(false);
      return;
    }
    REXLOG_INFO("cod3: function profile of {} ({}) for {} s: started", row.name, row.role, seconds);
    const auto t0 = Clock::now();
    double cpu_seconds = 0;
    const bool ok = Capture(row.id, seconds, &cpu_seconds);
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    if (!ok) {
      REXLOG_WARN("cod3: function profile of {}: the sampler could not start", row.name);
    } else {
      const uint32_t n = std::min(g_sample_count.load(), kMaxSamples);
      std::vector<uintptr_t> pcs(n);
      for (uint32_t i = 0; i < n; ++i) pcs[i] = g_samples[i].load(std::memory_order_relaxed);
      const double cpu_share = std::clamp(cpu_seconds / std::max(0.001, secs), 0.0, 1.0);
      if (WriteProfile(row, secs, cpu_share, pcs).empty())
        REXLOG_WARN("cod3: function profile of {}: could not write the report", row.name);
    }
    g_busy.store(false);
  }).detach();

  const std::string who =
      known_now ? row.name + " (" + (row.role.empty() ? std::string("no known role") : row.role) + ")"
                : "\"" + wanted + "\"";
  return "Profiling " + who + " for " + std::to_string(seconds) + " s, starting in " +
         std::to_string(delay_seconds) +
         " s: close the console now. The report goes to userdata/logs/profile-*.txt.";
#endif
}

}  // namespace cod3

// The F10 log console's command (the ~ game console sets cod3_ settings, not
// commands): cod3_profile [main|gpu|window|<name>] [seconds].
namespace {
void ProfileCommand(std::string_view args) {
  std::istringstream in{std::string(args)};
  std::string thread;
  std::string seconds_text;
  in >> thread >> seconds_text;
  int seconds = 10;
  if (!seconds_text.empty()) seconds = std::atoi(seconds_text.c_str());
  REXLOG_INFO("cod3: {}", cod3::FunctionProfiler::Start(thread.empty() ? "main" : thread,
                                                        seconds > 0 ? seconds : 10, 3));
}
}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(cod3_profile, ProfileCommand, "cod3/Performance",
                            "cod3_profile [main|gpu|window] [seconds]: which functions a thread "
                            "spends its time in, written to userdata/logs/profile-*.txt; close "
                            "this console within 3 s");
