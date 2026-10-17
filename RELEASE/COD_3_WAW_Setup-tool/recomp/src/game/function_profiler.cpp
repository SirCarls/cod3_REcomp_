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
#include <rex/thread/mutex.h>

#include "game/diagnostics.h"
#include "game/thread_profiler.h"

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif
#include <cxxabi.h>
#include <elf.h>
#include <link.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>  // K32EnumProcessModules, in kernel32 since Windows 7
#endif

// The recompiled code's guest-to-host function table (generated/default/
// cod3_init.cpp): every game function's Xbox 360 address and the host
// function it became, ending with {0, nullptr}.
extern PPCFuncMapping PPCFuncMappings[];

namespace cod3 {

namespace {

using Clock = std::chrono::steady_clock;

// Samples are instruction addresses, written by the signal handler (Linux)
// or the sampling thread (Windows). 120 s at 1 kHz fits. Each also keeps up
// to kCandidates words from the thread's stack that point into code, nearest
// the stack pointer first: one of them is usually the return address, which
// names the caller of code that has no symbol of its own (memcpy and other
// library routines, a graphics driver). In a function that hasn't set up a
// frame it's the first; one that has pushes registers and keeps locals
// above it.
constexpr uint32_t kMaxSamples = 128 * 1024;
constexpr size_t kCandidates = 4;
constexpr size_t kStackWords = 48;  // looked at per sample: 384 bytes
std::unique_ptr<std::atomic<uintptr_t>[]> g_samples;
std::unique_ptr<std::atomic<uintptr_t>[]> g_candidates;  // kCandidates per sample; 0: none
// Per sample: whether the thread held the SDK's global lock then (Linux with
// glibc, which keeps the lock owner's thread id in the mutex; 0 elsewhere).
std::unique_ptr<std::atomic<uint8_t>[]> g_held_lock;
#if defined(__linux__) && defined(__GLIBC__)
#define COD3_SEES_LOCK_OWNER 1
pthread_mutex_t* g_global_lock = nullptr;  // set before a capture starts
std::atomic<int> g_sampled_tid{0};
bool HoldsGlobalLock() {
  return g_global_lock && __atomic_load_n(&g_global_lock->__data.__owner, __ATOMIC_RELAXED) ==
                              g_sampled_tid.load(std::memory_order_relaxed);
}
#else
#define COD3_SEES_LOCK_OWNER 0
bool HoldsGlobalLock() { return false; }
#endif
std::atomic<uint32_t> g_sample_count{0};
std::atomic<bool> g_busy{false};

// The loaded modules' code, sorted by address, so a sample keeps only the
// stack words that point into code. Filled before a capture starts and only
// read while it runs (from the signal handler too: no locks).
constexpr size_t kMaxCodeRanges = 512;
std::pair<uintptr_t, uintptr_t> g_code[kMaxCodeRanges];  // [begin, end)
std::atomic<size_t> g_code_count{0};

bool InCode(uintptr_t address) {
  const size_t count = g_code_count.load(std::memory_order_acquire);
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (g_code[mid].second <= address)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < count && address >= g_code[lo].first;
}

// This session's profiles, a few lines each, for cod3_report.
std::mutex g_summaries_mutex;
std::vector<std::string> g_summaries;

// Profile reports kept in the logs folder; older ones are deleted.
constexpr size_t kKeepProfiles = 30;

// From the signal handler as well: atomics and plain reads only.
void RecordSample(uintptr_t pc, const uintptr_t* stack, size_t words) {
  const uint32_t i = g_sample_count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxSamples) return;
  g_samples[i].store(pc, std::memory_order_relaxed);
  g_held_lock[i].store(HoldsGlobalLock() ? 1 : 0, std::memory_order_relaxed);
  std::atomic<uintptr_t>* out = &g_candidates[size_t(i) * kCandidates];
  size_t kept = 0;
  for (size_t w = 0; w < words && kept < kCandidates; ++w)
    if (InCode(stack[w])) out[kept++].store(stack[w], std::memory_order_relaxed);
  for (; kept < kCandidates; ++kept) out[kept].store(0, std::memory_order_relaxed);
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
  // "#<OS id>": that thread exactly (the automatic profile of a lock holder).
  const uint32_t id = w.size() > 1 && w[0] == '#' ? uint32_t(std::strtoul(w.c_str() + 1, nullptr, 10)) : 0;
  for (const ThreadRow& row : report.rows) {
    if (id ? row.id == id : role ? row.role == role : Lower(row.name).rfind(w, 0) == 0) {
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
  bool size_estimated = false;  // named from an address only (AddCLibraryNames)
};

struct Module {
  std::string path;   // file
  std::string label;  // what the report calls it
  uintptr_t base = 0;
  std::vector<std::pair<uintptr_t, uintptr_t>> code;  // [begin, end) ranges
  std::vector<Symbol> symbols;                        // sorted by address
  bool symbols_read = false;
  std::string build_id;     // hex, from the GNU build-id note (Linux)
  bool debug_file = false;  // symbols came from the separate debug file
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
  // The file name, by hand: std::filesystem's conversions can throw on
  // Windows for names outside the system code page.
  const size_t slash = path.find_last_of("/\\");
  const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
  if (file.rfind("librexruntime", 0) == 0 || file.rfind("rexruntime", 0) == 0)
    return "ReXGlue runtime (" + file + ")";
  if (file.rfind("librexgpu", 0) == 0 || file.rfind("rexgpu", 0) == 0)
    return "ReXGlue GPU emulation (" + file + ")";
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

// The GNU build-id in a loaded note segment, as hex ("" when there's none):
// it names the exact build of a library, and its separate debug file.
std::string BuildId(uintptr_t notes, uintptr_t size, uintptr_t align) {
  align = align == 8 ? 8 : 4;
  auto padded = [align](uintptr_t n) { return (n + align - 1) & ~(align - 1); };
  const uintptr_t end = notes + size;
  for (uintptr_t p = notes; p + sizeof(ElfW(Nhdr)) <= end;) {
    const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
    const uintptr_t name = p + sizeof(ElfW(Nhdr));
    const uintptr_t desc = name + padded(note->n_namesz);
    const uintptr_t next = desc + padded(note->n_descsz);
    if (next > end || next <= p) break;
    if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
        std::memcmp(reinterpret_cast<const void*>(name), "GNU", 4) == 0) {
      std::string hex;
      for (uint32_t i = 0; i < note->n_descsz; ++i) {
        char byte[3];
        std::snprintf(byte, sizeof(byte), "%02x", reinterpret_cast<const uint8_t*>(desc)[i]);
        hex += byte;
      }
      return hex;
    }
    p = next;
  }
  return {};
}

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
          if (ph.p_type == PT_NOTE) m.build_id += BuildId(m.base + ph.p_vaddr, ph.p_memsz, ph.p_align);
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

// Function symbols from an ELF file's .symtab (or its .dynsym when it has no
// .symtab and `dynamic_too`). Returns whether the full .symtab was there.
bool ReadSymbolFile(Module& m, const std::string& path, bool dynamic_too) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  ElfW(Ehdr) eh{};
  if (!f.read(reinterpret_cast<char*>(&eh), sizeof(eh)) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != (sizeof(void*) == 8 ? ELFCLASS64 : ELFCLASS32) ||
      eh.e_shentsize != sizeof(ElfW(Shdr)))
    return false;
  std::vector<ElfW(Shdr)> sections(eh.e_shnum);
  f.seekg(std::streamoff(eh.e_shoff));
  if (!f.read(reinterpret_cast<char*>(sections.data()), std::streamsize(sections.size() * sizeof(ElfW(Shdr)))))
    return false;
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
  if (read_table(SHT_SYMTAB)) return true;
  if (dynamic_too) read_table(SHT_DYNSYM);
  return false;
}

#if defined(__GLIBC__)
// The C library's memory and string routines have one version per CPU
// family (SSE2, AVX2, AVX-512 ...), picked when the library loads, and none
// of them is in the library's own (dynamic) symbol table: without its debug
// symbols, the time spent copying memory showed as C library code without a
// name. This program's own references to them hold the picked versions'
// addresses (the dynamic loader resolves them), so those get the routine's
// name. Each one's extent is a guess (up to the next known start, at most
// 4 KB); the report says so.
void AddCLibraryNames(Module& m) {
  const std::pair<const char*, uintptr_t> routines[] = {
      {"memcpy", reinterpret_cast<uintptr_t>(&::memcpy)},
      {"memmove", reinterpret_cast<uintptr_t>(&::memmove)},
      {"memset", reinterpret_cast<uintptr_t>(&::memset)},
      {"memcmp", reinterpret_cast<uintptr_t>(&::memcmp)},
      {"strlen", reinterpret_cast<uintptr_t>(&::strlen)},
      {"strcmp", reinterpret_cast<uintptr_t>(&::strcmp)},
      {"strncmp", reinterpret_cast<uintptr_t>(&::strncmp)},
  };
  auto in_code = [&m](uintptr_t a) {
    for (const auto& r : m.code)
      if (a >= r.first && a < r.second) return true;
    return false;
  };
  auto named = [&m](uintptr_t a) {
    for (const Symbol& s : m.symbols)
      if (a >= s.address && a < s.address + std::max<uintptr_t>(s.size, 1)) return true;
    return false;
  };
  std::vector<Symbol> added;
  for (const auto& [name, address] : routines) {
    if (!in_code(address) || named(address)) continue;
    auto same = std::find_if(added.begin(), added.end(), [&](const Symbol& s) { return s.address == address; });
    if (same != added.end()) {
      same->name += std::string(" / ") + name;  // memcpy and memmove are often one routine
    } else {
      added.push_back({address, 0, name, true});
    }
  }
  m.symbols.insert(m.symbols.end(), added.begin(), added.end());
}
#endif

// A module's function symbols: its file's, or its separate debug file's
// when the file only has the dynamic ones and the debug file is installed
// (/usr/lib/debug/.build-id/, the layout Debian, Ubuntu and Fedora use).
void ReadSymbols(Module& m) {
  m.symbols_read = true;
  const bool full = ReadSymbolFile(m, m.path, true);
  if (!full && m.build_id.size() > 2) {
    const std::string debug_path =
        "/usr/lib/debug/.build-id/" + m.build_id.substr(0, 2) + "/" + m.build_id.substr(2) + ".debug";
    std::vector<Symbol> dynamic = std::move(m.symbols);
    m.symbols.clear();
    if (ReadSymbolFile(m, debug_path, false))
      m.debug_file = true;
    else
      m.symbols = std::move(dynamic);
  }
#if defined(__GLIBC__)
  if (m.label.rfind("C / C++ library (libc.so", 0) == 0) AddCLibraryNames(m);
#endif
  std::sort(m.symbols.begin(), m.symbols.end(),
            [](const Symbol& a, const Symbol& b) { return a.address < b.address; });
  // The extent of a routine named from its address only: up to the next
  // symbol's start, at most 4 KB.
  for (size_t i = 0; i < m.symbols.size(); ++i) {
    Symbol& s = m.symbols[i];
    if (!s.size_estimated) continue;
    uintptr_t next = s.address + 4096;
    for (size_t j = i + 1; j < m.symbols.size(); ++j)
      if (m.symbols[j].address > s.address) {
        next = std::min(next, m.symbols[j].address);
        break;
      }
    s.size = next - s.address;
  }
}

#elif defined(_WIN32)

// The loaded images, whole (code and data: fine for telling code addresses on
// the stack from heap and stack pointers). No symbols are read: the game's
// functions are named from the guest-to-host table.
std::string Utf8(const wchar_t* text) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::vector<Module> LoadedModules() {
  std::vector<Module> modules;
  HMODULE handles[1024];
  DWORD needed = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) return modules;
  const HMODULE exe = GetModuleHandleW(nullptr);
  for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
    MODULEINFO info{};
    wchar_t path[MAX_PATH];
    if (!K32GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)) ||
        !GetModuleFileNameW(handles[i], path, MAX_PATH))
      continue;
    Module m;
    m.path = Utf8(path);
    m.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    m.code.push_back({m.base, m.base + info.SizeOfImage});
    m.label = handles[i] == exe ? "game code (cod3)" : ModuleLabel(m.path);
    m.symbols_read = true;
    modules.push_back(std::move(m));
  }
  return modules;
}
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
  bool size_estimated = false;  // see AddCLibraryNames
  uintptr_t start = 0;          // the function's first instruction, when known
};

// Places whose callers the report lists: code without a name, and library
// and driver code (who copies all that memory matters more than memcpy).
bool WantsCallers(const Place& p) {
  return p.name == "(no symbol)" || p.module.rfind("C / C++ library", 0) == 0 ||
         p.module.rfind("graphics driver", 0) == 0;
}

// ----------------------------------------------------------------- report --

std::filesystem::path WriteProfile(const ThreadRow& thread, double seconds, double cpu_share,
                                   const std::vector<uintptr_t>& pcs,
                                   const std::vector<uintptr_t>& candidates,
                                   const std::vector<uint8_t>& held_lock) {
  std::vector<Module> modules = LoadedModules();
  const GuestTable guest_table;

  auto module_of = [&](uintptr_t pc) -> Module* {
    for (Module& m : modules)
      for (const auto& r : m.code)
        if (pc >= r.first && pc < r.second) return &m;
    return nullptr;
  };
  auto resolve = [&](uintptr_t pc) {
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
          p.size_estimated = it->size_estimated;
          p.start = it->address;
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
        p.start = start;
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
        p.module = ModuleLabel(Utf8(path));
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
    return p;
  };
  // Whether `ret` looks like a return address: inside a module's code, right
  // after a call instruction. x86-64: E8 rel32, FF 15 [rip+disp32], FF /2 on
  // a register, [reg+disp8] or [reg+disp32], with or without a REX prefix.
  // ARM64: BL or BLR.
  auto is_return_address = [&](uintptr_t ret) {
    Module* mod = module_of(ret);
    if (!mod) return false;
    bool in_range = false;
    for (const auto& r : mod->code)
      if (ret - 8 >= r.first && ret <= r.second) in_range = true;
    if (!in_range) return false;
    const auto* b = reinterpret_cast<const uint8_t*>(ret);
#if defined(__x86_64__) || defined(_M_X64)
    if (b[-5] == 0xE8) return true;                                       // call rel32
    if (b[-6] == 0xFF && b[-5] == 0x15) return true;                      // call [rip+disp32]
    if (b[-2] == 0xFF && (b[-1] & 0xF8) == 0xD0) return true;             // call reg
    if (b[-2] == 0xFF && (b[-1] & 0xF8) == 0x10 && (b[-1] & 6) != 4) return true;  // call [reg]
    if (b[-3] == 0xFF && (b[-2] & 0xF8) == 0x50) return true;             // call [reg+disp8]
    if (b[-6] == 0xFF && (b[-5] & 0xF8) == 0x90) return true;             // call [reg+disp32]
    // The same with a SIB byte ([base+index*scale], which code built for the
    // large code model uses for every call through its GOT).
    if (b[-3] == 0xFF && (b[-2] & 0xF8) == 0x10 && (b[-2] & 7) == 4) return true;
    if (b[-4] == 0xFF && (b[-3] & 0xF8) == 0x50 && (b[-3] & 7) == 4) return true;
    if (b[-7] == 0xFF && (b[-6] & 0xF8) == 0x90 && (b[-6] & 7) == 4) return true;
    return false;
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint32_t insn = 0;
    std::memcpy(&insn, b - 4, sizeof(insn));
    return (insn & 0xFC000000u) == 0x94000000u ||  // bl
           (insn & 0xFFFFFC1Fu) == 0xD63F0000u;    // blr
#else
    (void)b;
    return true;
#endif
  };

  std::map<std::string, Place> places;      // by name + module
  std::map<std::string, uint64_t> by_module;
  // Library, driver and unnamed code: who called it (when the sample shows),
  // and where in its module it was when it has no exact name (module
  // offsets, for matching the library's build later).
  std::map<std::string, std::map<std::string, uint64_t>> callers_of;
  std::map<std::string, std::map<uintptr_t, uint64_t>> offsets_of;
  bool estimated_names = false;
  // A sample's function and its callers as far as the stack words show:
  // "A <- B <- C".
  auto chain_of = [&](size_t i, const Place& p) {
    std::string chain = p.name, last = p.name;
    int links = 0;
    for (size_t c = 0; c < kCandidates && links < 3 && i * kCandidates + c < candidates.size(); ++c) {
      const uintptr_t word = candidates[i * kCandidates + c];
      if (!word || !is_return_address(word)) continue;
      const std::string name = resolve(word).name;
      if (name == last) continue;
      chain += " <- " + name;
      last = name;
      ++links;
    }
    return chain;
  };
  // The samples taken while the thread held the SDK's global lock, by chain:
  // what it does while other threads that want the lock wait.
  uint64_t held_samples = 0;
  std::map<std::string, uint64_t> held_chains;
  for (size_t i = 0; i < pcs.size(); ++i) {
    const uintptr_t pc = pcs[i];
    const Place p = resolve(pc);
    by_module[p.module]++;
    const std::string key = p.module + "\n" + p.name;
    Place& slot = places[key];
    if (slot.samples == 0) slot = p;
    slot.samples++;
    if (p.size_estimated) estimated_names = true;
    if (i < held_lock.size() && held_lock[i]) {
      ++held_samples;
      held_chains[chain_of(i, p)]++;
    }
    if (WantsCallers(p)) {
      // The first candidate right after a call instruction.
      uintptr_t ret = 0;
      for (size_t c = 0; c < kCandidates && i * kCandidates + c < candidates.size(); ++c) {
        const uintptr_t word = candidates[i * kCandidates + c];
        if (word && is_return_address(word)) {
          ret = word;
          break;
        }
      }
      std::string caller = "(caller not known)";
      if (ret) {
        const Place c = resolve(ret);
        caller = c.name;
        if (c.start) {
          // Where in the caller: one function can call the same routine from
          // several places.
          char offset[32];
          std::snprintf(offset, sizeof(offset), " +0x%" PRIxPTR, ret - c.start);
          caller += offset;
        }
        caller += " in " + c.module;
        if (c.guest) {
          char guest[24];
          std::snprintf(guest, sizeof(guest), " [0x%08X]", c.guest);
          caller += guest;
        }
      }
      callers_of[key][caller]++;
      if (p.name == "(no symbol)" || p.size_estimated)
        if (Module* mod = module_of(pc)) offsets_of[key][(pc - mod->base) & ~uintptr_t(15)]++;
    }
  }

  std::vector<Place> ranked;
  for (auto& [key, place] : places) ranked.push_back(place);
  std::sort(ranked.begin(), ranked.end(), [](const Place& a, const Place& b) { return a.samples > b.samples; });
  std::vector<std::pair<std::string, uint64_t>> modules_ranked(by_module.begin(), by_module.end());
  std::sort(modules_ranked.begin(), modules_ranked.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  std::vector<std::pair<uint64_t, std::string>> held_ranked;
  for (const auto& [chain, n] : held_chains) held_ranked.push_back({n, chain});
  std::sort(held_ranked.rbegin(), held_ranked.rend());

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

#if COD3_SEES_LOCK_OWNER
  // What the thread did while it held the SDK's global lock (any thread that
  // wants the lock waits meanwhile; the thread table says who waits for whom).
  out << "\n== While it held the SDK's global lock ==\n";
  if (held_samples == 0) {
    out << "It didn't hold it in any sample.\n";
  } else {
    std::snprintf(line, sizeof(line),
                  "%.1f%% of the samples. Each line: the function, then its callers as far as the\n"
                  "stack shows (a good guess, not a certainty).\n",
                  100.0 * double(held_samples) / total);
    out << line;
    for (size_t i = 0; i < held_ranked.size() && i < 20; ++i) {
      std::snprintf(line, sizeof(line), "%6.1f%%  ", 100.0 * double(held_ranked[i].first) / total);
      out << line << held_ranked[i].second << "\n";
    }
  }
#endif

  // Library, driver and unnamed code: who called it.
  bool callers_header = false;
  for (const Place& p : ranked) {
    if (!WantsCallers(p) || double(p.samples) / total < 0.01) continue;
    const std::string key = p.module + "\n" + p.name;
    if (!callers_header) {
      out << "\n== Library, driver and unnamed code, by caller ==\n"
             "(The caller is the first word on the stack that points right after a call\n"
             "instruction: a good guess, not a certainty.)\n";
      callers_header = true;
    }
    std::snprintf(line, sizeof(line), "%6.1f%%  %s  %s\n", 100.0 * double(p.samples) / total, p.name.c_str(),
                  p.module.c_str());
    out << line;
    std::vector<std::pair<uint64_t, std::string>> callers;
    for (const auto& [caller, n] : callers_of[key]) callers.push_back({n, caller});
    std::sort(callers.rbegin(), callers.rend());
    for (size_t i = 0; i < callers.size() && i < 8; ++i) {
      std::snprintf(line, sizeof(line), "    %5.1f%%  from %s\n",
                    100.0 * double(callers[i].first) / total, callers[i].second.c_str());
      out << line;
    }
    std::vector<std::pair<uint64_t, uintptr_t>> offsets;
    for (const auto& [offset, n] : offsets_of[key]) offsets.push_back({n, offset});
    std::sort(offsets.rbegin(), offsets.rend());
    std::string list;
    for (size_t i = 0; i < offsets.size() && i < 6; ++i) {
      char part[48];
      std::snprintf(part, sizeof(part), "%s+0x%" PRIxPTR " %.1f%%", i ? ", " : "", offsets[i].second,
                    100.0 * double(offsets[i].first) / total);
      list += part;
    }
    if (!list.empty()) out << "    at " << list << "\n";
  }
  if (estimated_names)
    out << "\n(memcpy and the other C library routines named without the library's debug\n"
           "symbols: the version this CPU uses, named from its start; where it ends is a guess.)\n";
#if defined(__GLIBC__)
  // Which C library build: its offsets above can be named later from that
  // build's debug symbols.
  out << "\n(C library: glibc " << gnu_get_libc_version();
  for (const Module& m : modules) {
    if (m.label.rfind("C / C++ library (libc.so", 0) != 0) continue;
    if (!m.build_id.empty()) out << ", build " << m.build_id;
    if (m.symbols_read) out << (m.debug_file ? ", its debug symbols read" : ", no debug symbols installed");
    break;
  }
  out << ")\n";
#endif

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

  // For the report: the modules and the eight hottest functions.
  {
    char head[256];
    std::snprintf(head, sizeof(head), "%02d:%02d:%02d  %s (%s), %.0f s, busy %.0f%%, %zu samples; %s\n",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, thread.name.c_str(),
                  thread.role.empty() ? "no known role" : thread.role.c_str(), seconds,
                  cpu_share * 100.0, pcs.size(), path.filename().string().c_str());
    std::string summary = head;
    summary += "  modules:";
    for (size_t i = 0; i < modules_ranked.size() && i < 4; ++i) {
      std::snprintf(line, sizeof(line), "%s %s %.0f%%", i ? "," : "", modules_ranked[i].first.c_str(),
                    100.0 * double(modules_ranked[i].second) / total);
      summary += line;
    }
    summary += "\n";
    if (held_samples > 0) {
      std::snprintf(line, sizeof(line), "  held the SDK's global lock in %.1f%% of the samples, most in: ",
                    100.0 * double(held_samples) / total);
      summary += line + held_ranked[0].second + "\n";
    }
    for (size_t i = 0; i < ranked.size() && i < 8; ++i) {
      char guest[24] = "";
      if (ranked[i].guest) std::snprintf(guest, sizeof(guest), " [0x%08X]", ranked[i].guest);
      std::snprintf(line, sizeof(line), "  %5.1f%%  %s%s\n", 100.0 * double(ranked[i].samples) / total,
                    ranked[i].name.c_str(), guest);
      summary += line;
    }
    std::lock_guard lock(g_summaries_mutex);
    g_summaries.push_back(std::move(summary));
  }

  // Keep the newest kKeepProfiles reports.
  {
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> old;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      const std::string file = e.path().filename().string();
      if (file.rfind("profile-", 0) == 0 && e.path().extension() == ".txt")
        old.push_back({e.last_write_time(ec), e.path()});
    }
    if (old.size() > kKeepProfiles) {
      std::sort(old.begin(), old.end());
      for (size_t i = 0; i + kKeepProfiles < old.size(); ++i) std::filesystem::remove(old[i].second, ec);
    }
  }
  return path;
}

// ---------------------------------------------------------------- capture --

// The code ranges the sampler checks stack words against (g_code), from the
// modules loaded now. Called before a capture starts, never during one.
void SetCodeRanges() {
  std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
  for (const Module& m : LoadedModules())
    for (const auto& r : m.code)
      if (r.first < r.second) ranges.push_back(r);
  std::sort(ranges.begin(), ranges.end());
  size_t n = 0;
  for (const auto& r : ranges) {
    if (n > 0 && r.first <= g_code[n - 1].second) {
      g_code[n - 1].second = std::max(g_code[n - 1].second, r.second);  // overlapping: one range
    } else if (n < kMaxCodeRanges) {
      g_code[n++] = r;
    }
  }
  g_code_count.store(n, std::memory_order_release);
}

#if defined(__linux__)

// The kernel's id for a thread's CPU-time clock (make_thread_cpuclock(tid,
// CPUCLOCK_SCHED) in the kernel's include/linux/posix-timers.h, what glibc's
// pthread_getcpuclockid builds for its own threads).
clockid_t ThreadCpuClock(pid_t tid) { return clockid_t((~unsigned(tid) << 3) | 6); }

// Up to `words` words of this thread's stack from `sp` into `out`; returns
// how many it got. A system call that fails instead of faulting where the
// stack ends (it can end within the words looked at); without it (a sandbox
// that forbids it), only the words up to the end of sp's page, which is
// always mapped.
std::atomic<bool> g_vm_read_works{true};
size_t ReadStack(uintptr_t sp, uintptr_t* out, size_t words) {
  if (g_vm_read_works.load(std::memory_order_relaxed)) {
    iovec local{out, words * sizeof(uintptr_t)};
    iovec remote{reinterpret_cast<void*>(sp), words * sizeof(uintptr_t)};
    const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (got >= 0) return size_t(got) / sizeof(uintptr_t);
    if (errno == ENOSYS || errno == EPERM) g_vm_read_works.store(false, std::memory_order_relaxed);
  }
  const size_t in_page = (4096 - (sp & 4095)) / sizeof(uintptr_t);
  const size_t n = std::min(words, in_page);
  std::memcpy(out, reinterpret_cast<const void*>(sp), n * sizeof(uintptr_t));
  return n;
}

void OnProfSignal(int, siginfo_t*, void* context) {
  const int saved_errno = errno;
  const auto* uc = static_cast<const ucontext_t*>(context);
  uintptr_t stack[kStackWords + 1];
#if defined(__x86_64__)
  const size_t words = ReadStack(uintptr_t(uc->uc_mcontext.gregs[REG_RSP]), stack, kStackWords);
  RecordSample(uintptr_t(uc->uc_mcontext.gregs[REG_RIP]), stack, words);
#elif defined(__aarch64__)
  // The link register first: a function that calls nothing keeps its
  // return address there.
  stack[0] = uintptr_t(uc->uc_mcontext.regs[30]);
  const size_t words = 1 + ReadStack(uintptr_t(uc->uc_mcontext.sp), stack + 1, kStackWords);
  RecordSample(uintptr_t(uc->uc_mcontext.pc), stack, words);
#else
  (void)uc;
  (void)stack;
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
#if COD3_SEES_LOCK_OWNER
  g_sampled_tid.store(int(tid), std::memory_order_relaxed);
#endif
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
  // A signal already on its way still finds the handler. After that, one
  // that is somehow later still is ignored rather than ending the game
  // (SIGPROF's default action).
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (!(previous.sa_flags & SA_SIGINFO) && previous.sa_handler == SIG_DFL) previous.sa_handler = SIG_IGN;
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
      // Its stack, read the way that fails instead of faulting where the
      // stack ends; failing that, up to the end of the stack pointer's page.
      auto read_stack = [](uintptr_t sp, uintptr_t* out, size_t words) -> size_t {
        SIZE_T read = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(sp), out,
                              words * sizeof(uintptr_t), &read))
          return size_t(read) / sizeof(uintptr_t);
        const size_t in_page = std::min(words, (4096 - (sp & 4095)) / sizeof(uintptr_t));
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(sp), out,
                                 in_page * sizeof(uintptr_t), &read)
                   ? size_t(read) / sizeof(uintptr_t)
                   : 0;
      };
      uintptr_t stack[kStackWords + 1];
#if defined(_M_X64) || defined(__x86_64__)
      RecordSample(uintptr_t(ctx.Rip), stack, read_stack(uintptr_t(ctx.Rsp), stack, kStackWords));
#elif defined(_M_ARM64) || defined(__aarch64__)
      stack[0] = uintptr_t(ctx.Lr);  // a function that calls nothing keeps its return address here
      RecordSample(uintptr_t(ctx.Pc), stack, 1 + read_stack(uintptr_t(ctx.Sp), stack + 1, kStackWords));
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

std::vector<std::string> FunctionProfiler::SessionSummaries() {
  std::lock_guard lock(g_summaries_mutex);
  return g_summaries;
}

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
  if (!g_candidates) g_candidates.reset(new std::atomic<uintptr_t>[size_t(kMaxSamples) * kCandidates]);
  if (!g_held_lock) g_held_lock.reset(new std::atomic<uint8_t>[kMaxSamples]);
#if COD3_SEES_LOCK_OWNER
  g_global_lock = rex::thread::global_critical_region::mutex().native_handle();
#endif
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
    SetCodeRanges();
    const auto t0 = Clock::now();
    double cpu_seconds = 0;
    const bool ok = Capture(row.id, seconds, &cpu_seconds);
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    if (!ok) {
      REXLOG_WARN("cod3: function profile of {}: the sampler could not start", row.name);
    } else {
      const uint32_t n = std::min(g_sample_count.load(), kMaxSamples);
      std::vector<uintptr_t> pcs(n), candidates(size_t(n) * kCandidates);
      for (uint32_t i = 0; i < n; ++i) pcs[i] = g_samples[i].load(std::memory_order_relaxed);
      for (size_t i = 0; i < candidates.size(); ++i)
        candidates[i] = g_candidates[i].load(std::memory_order_relaxed);
      std::vector<uint8_t> held_lock(n);
      for (uint32_t i = 0; i < n; ++i) held_lock[i] = g_held_lock[i].load(std::memory_order_relaxed);
      const double cpu_share = std::clamp(cpu_seconds / std::max(0.001, secs), 0.0, 1.0);
      if (WriteProfile(row, secs, cpu_share, pcs, candidates, held_lock).empty())
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
