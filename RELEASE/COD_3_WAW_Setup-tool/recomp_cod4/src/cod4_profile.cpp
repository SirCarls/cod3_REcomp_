// cod4 / cod4mp - a sampling profile of one or a few threads (see cod4_profile.h). [acct2-cod4]

#include "cod4_profile.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(cod4_debug_profile_thread, "", "cod4",
                      "Debug: sample which code the thread whose name starts with this spends its time in (e.g. "
                      "Backend, the game's render thread; up to 4 names with commas, e.g. Backend,GPU), about "
                      "1,000 times a second each; the counts go to logs/profile-<name>.txt. Linux only");
REXCVAR_DEFINE_INT32(cod4_debug_profile_start, 45, "cod4",
                     "Debug: seconds after start before cod4_debug_profile_thread's sampling begins");
REXCVAR_DEFINE_INT32(cod4_debug_profile_seconds, 10, "cod4",
                     "Debug: how many seconds cod4_debug_profile_thread samples (1 to 60)");

#if defined(__linux__)

#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace cod4::profile {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxThreads = 4;
constexpr uint32_t kMaxSamples = 1u << 16;  // a thread

// One sampled thread. The signal handler finds its own by the thread id.
struct Target {
  std::atomic<pid_t> tid{0};
  std::atomic<uint32_t> count{0};
  uint64_t samples[kMaxSamples];  // written by the signal handler only, read after sampling ends
  std::string prefix;             // what was asked for
  std::string name;               // the thread's name
};

Target g_targets[kMaxThreads];
std::thread g_thread;
std::atomic<bool> g_stop{false};

// The signal handler, on the sampled thread: the address it was running.
void OnSample(int, siginfo_t*, void* context) {
  const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
  const uint64_t pc = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
  const uint64_t pc = uint64_t(uc->uc_mcontext.pc);
#else
  const uint64_t pc = 0;
#endif
  const pid_t self = pid_t(syscall(SYS_gettid));
  for (Target& target : g_targets) {
    if (target.tid.load(std::memory_order_relaxed) != self) continue;
    const uint32_t i = target.count.fetch_add(1, std::memory_order_relaxed);
    if (i < kMaxSamples) target.samples[i] = pc;
    return;
  }
}

// The thread whose name starts with `prefix` (its /proc/self/task/<tid>/comm); 0 when there is none.
pid_t FindThread(const std::string& prefix, std::string* name) {
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return 0;
  pid_t found = 0;
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    std::ifstream comm(std::string("/proc/self/task/") + entry->d_name + "/comm");
    std::string thread_name;
    std::getline(comm, thread_name);
    if (thread_name.compare(0, prefix.size(), prefix) == 0) {
      found = pid_t(std::atoi(entry->d_name));
      *name = thread_name;
      break;
    }
  }
  closedir(dir);
  return found;
}

// Sleeps up to `ms`, in steps, and says whether to go on (false once Stop() was called).
bool Wait(int64_t ms) {
  for (int64_t waited = 0; waited < ms && !g_stop.load(); waited += 50)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return !g_stop.load();
}

std::string SafeName(std::string text) {
  for (char& c : text)
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  return text;
}

// The names asked for, split at commas (empty ones dropped, at most kMaxThreads).
std::vector<std::string> SplitNames(const std::string& text) {
  std::vector<std::string> names;
  size_t begin = 0;
  while (begin <= text.size() && int(names.size()) < kMaxThreads) {
    size_t end = text.find(',', begin);
    if (end == std::string::npos) end = text.size();
    if (end > begin) names.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return names;
}

void Report(const std::filesystem::path& log_dir, const Target& target, double took);

void Run(std::filesystem::path log_dir, std::string prefixes, int start_s, int seconds) {
  if (!Wait(int64_t(start_s) * 1000)) return;
  int targets = 0;
  for (const std::string& prefix : SplitNames(prefixes)) {
    std::string name;
    const pid_t tid = FindThread(prefix, &name);
    bool taken = false;
    for (int k = 0; k < targets; ++k) taken = taken || g_targets[k].tid.load() == tid;
    if (tid == 0 || taken) {
      REXLOG_WARN("cod4: profile: no thread (or none not already sampled) whose name starts with \"{}\"", prefix);
      continue;
    }
    Target& target = g_targets[targets++];
    target.prefix = prefix;
    target.name = name;
    target.count.store(0);
    target.tid.store(tid);
  }
  if (targets == 0) return;
  // The handler stays installed afterwards: a signal still on its way must not meet SIGPROF's default (exit).
  struct sigaction action {};
  action.sa_sigaction = OnSample;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPROF, &action, nullptr) != 0) {
    REXLOG_WARN("cod4: profile: SIGPROF not available");
    return;
  }
  for (int k = 0; k < targets; ++k)
    REXLOG_INFO("cod4: profile: sampling thread {} ({}) for {} s", g_targets[k].tid.load(), g_targets[k].name, seconds);
  const pid_t pid = getpid();
  const auto start = Clock::now();
  const auto end = start + std::chrono::seconds(seconds);
  while (Clock::now() < end && !g_stop.load()) {
    for (int k = 0; k < targets; ++k) syscall(SYS_tgkill, pid, g_targets[k].tid.load(), SIGPROF);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (g_stop.load()) return;
  const double took = std::chrono::duration<double>(Clock::now() - start).count();
  for (int k = 0; k < targets; ++k) Report(log_dir, g_targets[k], took);
}

// One thread's samples, by module and offset, to logs/profile-<prefix>.txt, and a line in the log.
void Report(const std::filesystem::path& log_dir, const Target& target, double took) {
  const uint32_t samples = std::min(target.count.load(), kMaxSamples);
  const pid_t tid = target.tid.load();
  const std::string& name = target.name;

  // module, offset (and the address itself, for a program that isn't position-independent) -> samples
  std::map<std::pair<std::string, uint64_t>, std::pair<uint32_t, uint64_t>> counts;
  std::map<std::string, uint32_t> by_module;
  for (uint32_t i = 0; i < samples; ++i) {
    const uint64_t pc = target.samples[i];
    Dl_info info{};
    std::string module = "?";
    uint64_t offset = pc;
    if (pc && dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
      module = std::filesystem::path(info.dli_fname).filename().string();
      offset = pc - reinterpret_cast<uint64_t>(info.dli_fbase);
    }
    auto& entry = counts[{module, offset}];
    ++entry.first;
    entry.second = pc;
    ++by_module[module];
  }
  std::vector<std::pair<uint32_t, std::pair<std::string, uint64_t>>> sorted;
  for (const auto& [key, value] : counts) sorted.push_back({value.first, key});
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

  std::error_code ec;
  std::filesystem::create_directories(log_dir, ec);
  const auto path = log_dir / ("profile-" + SafeName(target.prefix) + ".txt");
  std::ofstream out(path);
  out << "# thread " << tid << " (" << name << "), " << samples << " samples over " << took << " s\n";
  out << "# count module offset address\n";
  char line[160];
  for (const auto& [count, key] : sorted) {
    std::snprintf(line, sizeof(line), "%u %s %llx %llx\n", count, key.first.c_str(),
                  static_cast<unsigned long long>(key.second),
                  static_cast<unsigned long long>(counts[key].second));
    out << line;
  }
  std::string modules;
  for (const auto& [module, count] : by_module)
    modules += (modules.empty() ? "" : ", ") + module + " " + std::to_string(count * 100 / std::max(samples, 1u)) + "%";
  REXLOG_INFO("cod4: profile: {} samples of {} in {:.1f} s ({}); counts in {}", samples, name, took, modules,
              path.string());
}

}  // namespace

void Start(const std::filesystem::path& log_dir) {
  const std::string prefix = REXCVAR_GET(cod4_debug_profile_thread);
  if (prefix.empty()) return;
  g_stop.store(false);
  g_thread = std::thread(Run, log_dir, prefix, std::max(0, int(REXCVAR_GET(cod4_debug_profile_start))),
                         std::clamp(int(REXCVAR_GET(cod4_debug_profile_seconds)), 1, 60));
}

void Stop() {
  g_stop.store(true);
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace cod4::profile

#else  // !__linux__

namespace cod4::profile {

void Start(const std::filesystem::path&) {
  if (!REXCVAR_GET(cod4_debug_profile_thread).empty()) REXLOG_WARN("cod4: profile: cod4_debug_profile_thread is Linux only");
}

void Stop() {}

}  // namespace cod4::profile

#endif
