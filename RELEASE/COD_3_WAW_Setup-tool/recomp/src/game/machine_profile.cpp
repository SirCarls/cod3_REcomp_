// cod3 - what this machine has (see machine_profile.h).

#include "game/machine_profile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace cod3::machine {

namespace {

std::string Gib(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return text;
}

#if !defined(_WIN32)
// "0-3,8,10-11" -> {0,1,2,3,8,10,11}. Bad input gives what parsed so far.
std::vector<int> ParseCpuList(std::string_view text) {
  std::vector<int> cpus;
  size_t i = 0;
  auto number = [&](int& out) {
    if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
    out = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9' && out < 1 << 20)
      out = out * 10 + (text[i++] - '0');
    return true;
  };
  while (i < text.size()) {
    int first = 0, last = 0;
    if (!number(first)) break;
    last = first;
    if (i < text.size() && text[i] == '-') {
      ++i;
      if (!number(last)) break;
    }
    for (int c = first; c <= last && c - first < 4096; ++c) cpus.push_back(c);
    if (i < text.size() && text[i] == ',') ++i;
    while (i < text.size() && (text[i] == '\n' || text[i] == ' ')) ++i;
  }
  return cpus;
}
#endif

std::string JoinCpus(const std::vector<int>& cpus) {
  // Back to the compact "0-3,8" form.
  std::string s;
  for (size_t i = 0; i < cpus.size();) {
    size_t j = i;
    while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
    if (!s.empty()) s += ',';
    s += std::to_string(cpus[i]);
    if (j > i) s += '-' + std::to_string(cpus[j]);
    i = j + 1;
  }
  return s;
}

// Keeps only the allowed CPUs of each group and drops groups left empty;
// sorts and removes duplicates.
std::vector<std::vector<int>> Restrict(const std::vector<std::vector<int>>& groups,
                                       const std::set<int>& allowed) {
  std::set<std::vector<int>> unique;
  for (const auto& g : groups) {
    std::vector<int> kept;
    for (int c : g)
      if (allowed.count(c)) kept.push_back(c);
    std::sort(kept.begin(), kept.end());
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    if (!kept.empty()) unique.insert(kept);
  }
  return {unique.begin(), unique.end()};
}

#if !defined(_WIN32)

std::string ReadFirstLine(const std::string& path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

// cgroup v2 CPU quota of this process, in CPUs (0 = none). "max 100000" =
// no limit; "350000 100000" = 3.5 CPUs.
double CgroupQuota() {
  std::ifstream in("/proc/self/cgroup");
  std::string line, path;
  while (std::getline(in, line))
    if (line.rfind("0::", 0) == 0) path = line.substr(3);
  if (path.empty() || path.find("..") != std::string::npos) return 0;
  // The limit can sit on the process's cgroup or any parent; the tightest wins.
  double quota = 0;
  for (std::string p = path;; p = p.substr(0, p.rfind('/'))) {
    const std::string text = ReadFirstLine("/sys/fs/cgroup" + p + "/cpu.max");
    long long max = 0, period = 0;
    if (std::sscanf(text.c_str(), "%lld %lld", &max, &period) == 2 && max > 0 && period > 0) {
      const double q = double(max) / double(period);
      if (quota == 0 || q < quota) quota = q;
    }
    if (p.empty() || p == "/") break;
    if (p.rfind('/') == std::string::npos) break;
  }
  return quota;
}

void ReadLinux(Profile& p) {
  utsname u{};
  if (uname(&u) == 0) p.os = std::string(u.sysname) + " " + u.release + " " + u.machine;

  {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("model name", 0) == 0) {
        const size_t colon = line.find(':');
        if (colon != std::string::npos) p.cpu_model = line.substr(colon + 2);
        break;
      }
    }
  }

  const std::vector<int> online = ParseCpuList(ReadFirstLine("/sys/devices/system/cpu/online"));
  p.logical_cpus = int(online.size());

  cpu_set_t set;
  CPU_ZERO(&set);
  std::set<int> allowed;
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    for (int c : online)
      if (c < CPU_SETSIZE && CPU_ISSET(c, &set)) allowed.insert(c);
  } else {
    allowed.insert(online.begin(), online.end());
  }
  p.allowed_cpus.assign(allowed.begin(), allowed.end());
  p.cpu_quota = CgroupQuota();

  std::vector<std::vector<int>> cores, l3;
  for (int c : online) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c);
    std::string siblings = ReadFirstLine(base + "/topology/core_cpus_list");
    if (siblings.empty()) siblings = ReadFirstLine(base + "/topology/thread_siblings_list");
    cores.push_back(siblings.empty() ? std::vector<int>{c} : ParseCpuList(siblings));
    // The highest cache level present (L3 on desktop CPUs).
    int best_level = 0;
    std::string best_list;
    for (int index = 0; index < 8; ++index) {
      const std::string cache = base + "/cache/index" + std::to_string(index);
      const std::string level = ReadFirstLine(cache + "/level");
      if (level.empty()) continue;
      const int lv = std::atoi(level.c_str());
      if (lv > best_level) {
        best_level = lv;
        best_list = ReadFirstLine(cache + "/shared_cpu_list");
      }
    }
    if (best_level >= 2 && !best_list.empty()) l3.push_back(ParseCpuList(best_list));
  }
  p.cores = Restrict(cores, allowed);
  p.l3_groups = Restrict(l3, allowed);

  // Intel hybrid CPUs list their E-cores in the "cpu_atom" event source
  // (tools/perf/Documentation/intel-hybrid.txt); /sys/devices/cpu_atom is
  // the same directory on most kernels.
  std::string atom = ReadFirstLine("/sys/bus/event_source/devices/cpu_atom/cpus");
  if (atom.empty()) atom = ReadFirstLine("/sys/devices/cpu_atom/cpus");
  for (int c : ParseCpuList(atom))
    if (allowed.count(c)) p.efficiency_cpus.push_back(c);

  std::ifstream mem("/proc/meminfo");
  std::string key;
  uint64_t kb = 0;
  std::string unit;
  while (mem >> key >> kb >> unit) {
    if (key == "MemTotal:") p.ram_total_bytes = kb * 1024;
    if (key == "MemAvailable:") p.ram_available_bytes = kb * 1024;
  }
}

#else  // _WIN32

void ReadWindows(Profile& p) {
  // OS version (GetVersionEx lies to programs without a manifest; ntdll doesn't).
  p.os = "Windows";
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto get_version = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (get_version && get_version(&v) == 0) {
      char text[64];
      std::snprintf(text, sizeof(text), "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion,
                    v.dwBuildNumber);
      p.os = text;
    }
    using WineVersionFn = const char*(CDECL*)();
    auto wine_version = reinterpret_cast<WineVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")));
    if (wine_version) {
      p.under_wine = true;
      const char* version = wine_version();
      p.os += std::string(" (Wine ") + (version ? version : "?") + ")";
    }
  }

  {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
      wchar_t name[128] = {};
      DWORD size = sizeof(name) - sizeof(wchar_t);
      DWORD type = 0;
      if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                           reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS &&
          type == REG_SZ) {
        char narrow[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        p.cpu_model = narrow;
        while (!p.cpu_model.empty() && p.cpu_model.back() == ' ') p.cpu_model.pop_back();
      }
      RegCloseKey(key);
    }
  }

  // Processor group 0 only (Windows keeps a process in one group unless it
  // asks otherwise; machines with more than 64 logical CPUs are rare).
  DWORD_PTR process_mask = 0, system_mask = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
  std::set<int> allowed;
  for (int c = 0; c < int(sizeof(DWORD_PTR) * 8); ++c) {
    if (system_mask & (DWORD_PTR(1) << c)) ++p.logical_cpus;
    if (process_mask & (DWORD_PTR(1) << c)) allowed.insert(c);
  }
  p.allowed_cpus.assign(allowed.begin(), allowed.end());

  auto mask_cpus = [](KAFFINITY mask) {
    std::vector<int> cpus;
    for (int c = 0; c < int(sizeof(KAFFINITY) * 8); ++c)
      if (mask & (KAFFINITY(1) << c)) cpus.push_back(c);
    return cpus;
  };
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
  std::vector<uint8_t> buffer(length);
  std::vector<std::vector<int>> cores, l3;
  // PROCESSOR_RELATIONSHIP is { BYTE Flags; BYTE EfficiencyClass; ... }.
  // Older MinGW headers still call the second byte Reserved, so it is read by
  // its place, which is the same in every version of the struct.
  auto efficiency_class = [](const PROCESSOR_RELATIONSHIP& core) {
    return reinterpret_cast<const BYTE*>(&core)[1];
  };
  if (length &&
      GetLogicalProcessorInformationEx(
          RelationAll, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
          &length)) {
    // Hybrid CPUs: P-cores have the highest efficiency class.
    BYTE max_class = 0;
    for (DWORD offset = 0; offset < length;) {
      auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
      if (info->Relationship == RelationProcessorCore)
        max_class = std::max(max_class, efficiency_class(info->Processor));
      offset += info->Size;
    }
    for (DWORD offset = 0; offset < length;) {
      auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
      if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount > 0 &&
          info->Processor.GroupMask[0].Group == 0) {
        const auto cpus = mask_cpus(info->Processor.GroupMask[0].Mask);
        cores.push_back(cpus);
        if (max_class > 0 && efficiency_class(info->Processor) < max_class)
          for (int c : cpus)
            if (allowed.count(c)) p.efficiency_cpus.push_back(c);
      } else if (info->Relationship == RelationCache && info->Cache.Level == 3 &&
                 info->Cache.GroupMask.Group == 0) {
        l3.push_back(mask_cpus(info->Cache.GroupMask.Mask));
      }
      offset += info->Size;
    }
  }
  p.cores = Restrict(cores, allowed);
  p.l3_groups = Restrict(l3, allowed);

  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    p.ram_total_bytes = mem.ullTotalPhys;
    p.ram_available_bytes = mem.ullAvailPhys;
  }
}

#endif

Profile Read() {
  Profile p;
#if defined(_WIN32)
  ReadWindows(p);
#else
  ReadLinux(p);
#endif
  // Never report nothing: one CPU, one core.
  if (p.allowed_cpus.empty()) p.allowed_cpus.push_back(0);
  if (p.logical_cpus < int(p.allowed_cpus.size())) p.logical_cpus = int(p.allowed_cpus.size());
  if (p.cores.empty())
    for (int c : p.allowed_cpus) p.cores.push_back({c});
  return p;
}

}  // namespace

int Profile::UsableCpus() const {
  int usable = int(allowed_cpus.size());
  if (cpu_quota > 0) usable = std::min(usable, std::max(1, int(std::floor(cpu_quota))));
  return std::max(1, usable);
}

std::string Profile::Summary() const {
  std::ostringstream s;
  s << (cpu_model.empty() ? "CPU" : cpu_model) << ": " << logical_cpus << " logical CPUs";
  if (int(allowed_cpus.size()) != logical_cpus) s << " (" << allowed_cpus.size() << " allowed)";
  if (cpu_quota > 0) s << " (quota " << cpu_quota << ")";
  s << ", " << cores.size() << " cores";
  if (l3_groups.size() > 1) s << " in " << l3_groups.size() << " cache groups";
  if (!efficiency_cpus.empty()) s << ", " << efficiency_cpus.size() << " on E-cores";
  s << "; RAM " << Gib(ram_total_bytes) << " (" << Gib(ram_available_bytes) << " free)";
  if (under_wine) s << "; under Wine/Proton";
  return s.str();
}

std::string Profile::Describe() const {
  std::ostringstream s;
  s << "OS: " << os << "\n";
  s << "CPU: " << (cpu_model.empty() ? "(not reported)" : cpu_model) << "\n";
  s << "Logical CPUs: " << logical_cpus << "; this process may use " << allowed_cpus.size() << " ("
    << JoinCpus(allowed_cpus) << ")";
  if (cpu_quota > 0) s << ", CPU quota " << cpu_quota;
  s << "; usable " << UsableCpus() << "\n";
  s << "Physical cores: " << cores.size() << " (logical CPUs per core:";
  for (const auto& c : cores) s << " " << JoinCpus(c);
  s << ")\n";
  s << "Shared-cache groups: " << l3_groups.size();
  for (const auto& g : l3_groups) s << " [" << JoinCpus(g) << "]";
  s << "\n";
  if (!efficiency_cpus.empty()) s << "E-cores: " << JoinCpus(efficiency_cpus) << "\n";
  s << "RAM: " << Gib(ram_total_bytes) << " total, " << Gib(ram_available_bytes)
    << " available at start\n";
  if (under_wine) s << "Running under Wine/Proton\n";
  return s.str();
}

const Profile& Get() {
  static const Profile profile = Read();
  return profile;
}

ThreadBudget Budget(const Profile& p) {
  ThreadBudget b;
  const int usable = p.UsableCpus();
  const int physical = p.PhysicalCores();
  // The frame passes through four threads in a row: the game's main thread,
  // its render job, the GPU emulation and presenting.
  b.frame_threads_own_cores = physical >= 4 && usable >= 4;
  b.spinning_allowed = usable >= 6;  // as the SDK's GPU thread already does (patch 0022)
  b.draw_workers = usable < 8 ? 0 : std::clamp(usable / 6, 1, 8);
  const int os_reserve = usable >= 8 ? 1 : 0;
  const int frame = b.frame_threads_own_cores ? 4 : 2;
  b.pool_threads = std::max(1, usable - frame - b.draw_workers - os_reserve);
  return b;
}

std::string ThreadBudget::Describe() const {
  std::ostringstream s;
  s << "frame threads on their own cores: " << (frame_threads_own_cores ? "yes" : "no")
    << "; draw workers " << draw_workers << "; shared pool " << pool_threads
    << "; busy-waiting " << (spinning_allowed ? "allowed" : "off");
  return s.str();
}

}  // namespace cod3::machine
