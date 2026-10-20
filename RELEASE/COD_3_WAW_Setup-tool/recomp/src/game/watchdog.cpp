// cod3 - hang watchdog and last-chance crash logger (see watchdog.h).

#include "game/watchdog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

#include "game/guest.h"
#include "game/program.h"
#include "game/screenshot.h"
#include "game/script_threads.h"

#if defined(_WIN32)
#include <csignal>

#include <windows.h>
#endif

#if defined(__linux__)
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>

#include <rex/exception_handler.h>
#endif

// The recompiled code's guest-to-host function table (generated/default/
// cod3_init.cpp): every game function's Xbox 360 address and the host
// function it became, ending with {0, nullptr}.
extern PPCFuncMapping PPCFuncMappings[];

REXCVAR_DEFINE_INT32(cod3_watchdog_seconds, 10, "cod3",
                     "Log what the game's main thread was doing when it makes no progress for "
                     "this many seconds (0 = off)")
    .range(0, 3600);

REXCVAR_DEFINE_INT32(cod3_test_crash_logger, 0, "cod3",
                     "Test switch, from the command line only: this many seconds after its first "
                     "frame the game reads guest address 0x10 on its main thread, as a bug would, "
                     "and ends, to check the crash log (0 = off)")
    .range(0, 600);

namespace cod3::watchdog {

namespace {

using Clock = std::chrono::steady_clock;

// Written by the game thread, read by the watchdog (and the crash logger).
std::atomic<uint64_t> g_marks{0};           // marks so far
std::atomic<const char*> g_where{nullptr};  // the last one
std::atomic<const PPCContext*> g_ctx{nullptr};
thread_local bool t_game_thread = false;

// Guest call sites of kernel calls that can block, by the return address the
// call leaves in lr (found in the generated code: `ctx.lr = ...` right before
// the call). A thread stuck in one of them shows that address.
struct CallSite {
  uint32_t lr;
  const char* what;
};
constexpr CallSite kCallSites[] = {
    {COD3_ADDR(0x823494F8, 0x82319200), "NtWaitForSingleObjectEx in sub_823494C0 (WaitForSingleObjectEx)"},
    {COD3_ADDR(0x823444FC, 0x823137CC), "NtWaitForSingleObjectEx in sub_82344420 (a file write finishing)"},
    {COD3_ADDR(0x82344654, 0x82313964), "NtWaitForSingleObjectEx in sub_82344558 (a file read finishing)"},
    {COD3_ADDR(0x82344494, 0x82313764), "NtWriteFile in sub_82344420"},
    {COD3_ADDR(0x823444E0, 0x823137B0), "NtWriteFile in sub_82344420"},
    {COD3_ADDR(0x82345630, 0x82314A00), "NtWriteFile in sub_823455E8"},
    {COD3_ADDR(0x823445D0, 0x823138E0), "NtReadFile in sub_82344558"},
    {COD3_ADDR(0x82344638, 0x82313948), "NtReadFile in sub_82344558"},
    {COD3_ADDR(0x823455B8, 0x82314988), "NtReadFile in sub_82345570"},
    {COD3_ADDR(0x82349740, 0x82318B90), "KeDelayExecutionThread in sub_823496E0 (Sleep)"},
    {COD3_ADDR(0x82345050, 0x82314420), "NtYieldExecution in sub_82345040"},
    {COD3_ADDR(0x822F4DB8, 0x822C3AD8), "VdSwap in sub_822F4C28 (presenting a frame)"},
    {COD3_ADDR(0x822C0FE8, 0x82291FD8), "KeWaitForSingleObject in sub_822C0F90"},
    {COD3_ADDR(0x822C10A8, 0x82292098), "KeWaitForSingleObject in sub_822C1058"},
    {COD3_ADDR(0x822C10D8, 0x822920C8), "KeWaitForSingleObject in sub_822C1058"},
    {COD3_ADDR(0x822C11AC, 0x8229219C), "KeWaitForMultipleObjects in sub_822C1108"},
    {COD3_ADDR(0x8230258C, 0x822D117C), "KeWaitForSingleObject in sub_823024D8"},
    {COD3_ADDR(0x82302620, 0x822D1210), "KeWaitForSingleObject in sub_823025B0"},
    {COD3_ADDR(0x82302D1C, 0x822D190C), "KeWaitForSingleObject in sub_82302C80"},
    {COD3_ADDR(0x821960B8, 0x8222C318), "XMsgInProcessCall in sub_82196070"},
    {COD3_ADDR(0x82195AC4, 0x8239083C), "XMsgInProcessCall in sub_82195A90"},
    {COD3_ADDR(0x82195BE4, 0), "XMsgInProcessCall in sub_82195BA8"},
    {COD3_ADDR(0x8219627C, 0x8222C63C), "XMsgInProcessCall in sub_82196188"},
    {COD3_ADDR(0x82195B60, 0), "XMsgStartIORequestEx in sub_82195B00"},
    {COD3_ADDR(0x82343B14, 0x82312C8C), "XMsgStartIORequest in sub_82343AD8"},
    {COD3_ADDR(0x82343BA8, 0x82312D20), "XMsgStartIORequest in sub_82343B58"},
    {COD3_ADDR(0x82343E9C, 0x823132FC), "XexLoadImage in sub_82343E80 (loading a level's code)"},
};

std::string KnownCall(uint32_t lr) {
  for (const auto& site : kCallSites)
    if (site.lr == lr) return std::string(" = after ") + site.what;
  return {};
}

// Another thread's register, read while that thread runs: a plain 8-byte read
// that may be a moment old, which is all a report needs.
uint64_t Peek(const uint64_t& value) { return *reinterpret_cast<const volatile uint64_t*>(&value); }

// The game thread's state, for a report.
std::string GuestRegisters() {
  const PPCContext* c = g_ctx.load(std::memory_order_acquire);
  if (!c) return "guest registers not known yet";
  const auto lr = static_cast<uint32_t>(Peek(c->lr));
  const auto r1 = static_cast<uint32_t>(Peek(c->r1.u64));
  const auto r3 = static_cast<uint32_t>(Peek(c->r3.u64));
  return fmt::format("guest lr {:08X}{}, r1 {:08X}, r3 {:08X}", lr, KnownCall(lr), r1, r3);
}

const char* LastMark() {
  const char* where = g_where.load(std::memory_order_relaxed);
  return where ? where : "(none yet)";
}

void Report(double seconds, bool again) {
  REXLOG_WARN("cod3: watchdog: the game's main thread has {}made no progress for {:.0f} s; the "
              "last point it passed: {}",
              again ? "still " : "", seconds, LastMark());
  REXLOG_WARN("cod3: watchdog:   {}", GuestRegisters());
  REXLOG_WARN("cod3: watchdog:   fibers: {}", cod3::ScriptThreadWatchLine());
  rex::FlushLogging();
  // At debug log detail, the picture it is stuck on (game/screenshot.h).
  cod3::shot::Request(cod3::shot::Kind::kAuto, "the game's main thread is stuck");
}

#if defined(__linux__)
void InstallCrashLoggerWhenReady();  // below
#endif

// The watchdog thread: once a second, has the game thread marked anything?
void Watch() {
#if defined(__linux__)
  InstallCrashLoggerWhenReady();
#endif
  constexpr auto kRepeat = std::chrono::seconds(60);
  constexpr int kMaxReports = 5;
  uint64_t seen = g_marks.load(std::memory_order_relaxed);
  auto since = Clock::now();
  auto last_report = since;
  int reports = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto now = Clock::now();
    const uint64_t marks = g_marks.load(std::memory_order_relaxed);
    if (marks != seen) {
      if (reports > 0) {
        REXLOG_INFO("cod3: watchdog: the game's main thread is moving again after {:.0f} s (at {})",
                    std::chrono::duration<double>(now - since).count(), LastMark());
      }
      seen = marks;
      since = now;
      reports = 0;
      continue;
    }
    const int limit = REXCVAR_GET(cod3_watchdog_seconds);
    if (limit <= 0 || marks == 0) continue;  // off, or the game hasn't started yet
    const double stuck = std::chrono::duration<double>(now - since).count();
    if (reports == 0 && stuck >= limit) {
      Report(stuck, false);
      reports = 1;
      last_report = now;
    } else if (reports > 0 && reports < kMaxReports && now - last_report >= kRepeat) {
      Report(stuck, true);
      ++reports;
      last_report = now;
    }
  }
}

// --- What a crash report says about the game's code (both platforms) -------

std::atomic_flag g_crash_logged;  // starts clear (C++20)

// The recompiled functions by host and by guest address, from the generated
// table; built once at start-up, so a report only searches them.
struct FunctionTables {
  std::vector<std::pair<uintptr_t, uint32_t>> by_host;  // host start, guest address
  std::vector<uint32_t> by_guest;                        // guest starts
};
const FunctionTables* g_tables = nullptr;  // kept for the whole run

void BuildFunctionTables() {
  auto* t = new FunctionTables;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    t->by_host.push_back({reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest)});
    t->by_guest.push_back(uint32_t(m->guest));
  }
  std::sort(t->by_host.begin(), t->by_host.end());
  std::sort(t->by_guest.begin(), t->by_guest.end());
  g_tables = t;
}

// The start of the host function a code address is in, from the unwind
// tables (0 when there is none): what tells recompiled code from the port's
// own, which the linker places between the recompiled files.
#if defined(__linux__)
struct UnwindBases {  // libgcc's struct dwarf_eh_bases
  void* tbase;
  void* dbase;
  void* func;
};
extern "C" const void* _Unwind_Find_FDE(void* pc, UnwindBases* bases);
uintptr_t FunctionStart(uintptr_t pc) {
  UnwindBases bases{};
  if (!_Unwind_Find_FDE(reinterpret_cast<void*>(pc), &bases)) return 0;
  return reinterpret_cast<uintptr_t>(bases.func);
}
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
uintptr_t FunctionStart(uintptr_t pc) {
  DWORD64 image_base = 0;
  const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(DWORD64(pc), &image_base, nullptr);
  return f ? uintptr_t(image_base + f->BeginAddress) : 0;
}
#else
uintptr_t FunctionStart(uintptr_t) { return 0; }
#endif

// "sub_8212C6A0+0x1f4": the recompiled game function a host code address is
// in, or "" when it isn't in one (the port's own code, the SDK, a library).
// A return address is looked up one byte back: a call can be a function's
// last instruction.
std::string RecompiledAt(uintptr_t pc, bool return_address) {
  if (!g_tables || g_tables->by_host.empty()) return {};
  const uintptr_t start = FunctionStart(return_address ? pc - 1 : pc);
  if (!start || start > pc) return {};
  const auto& v = g_tables->by_host;
  auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(start, uint32_t(0)));
  if (it == v.end() || it->first != start) return {};
  return fmt::format("sub_{:08X}+{:#x}", it->second, pc - start);
}

// "8212C894 (sub_8212C6A0+0x1f4)": a guest code address and the function
// that starts nearest below it.
std::string GuestCode(uint32_t addr) {
  if (g_tables && !g_tables->by_guest.empty()) {
    const auto& v = g_tables->by_guest;
    auto it = std::upper_bound(v.begin(), v.end(), addr);
    if (it != v.begin() && addr - *(it - 1) < 0x10000)
      return fmt::format("{:08X} (sub_{:08X}+{:#x})", addr, *(it - 1), addr - *(it - 1));
  }
  return fmt::format("{:08X}", addr);
}

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}

// cod3_test_crash_logger N (command line only): N seconds after the game's
// first frame, a read of guest address 0x10 on its main thread (an unmapped
// page, as a null pointer in the game's code would read), to check what the
// log says. Set anywhere else (cod3.toml, the console) it is refused, so it
// can't end anyone's game by surprise.
void TestCrashIfAsked() {
  const int after = REXCVAR_GET(cod3_test_crash_logger);
  if (after <= 0) return;
  static const auto first = Clock::now();
  static std::atomic<int> state{0};  // 0 not yet looked at, 1 armed, 2 refused
  if (state.load(std::memory_order_relaxed) == 0) {
    const bool ok =
        rex::cvar::GetFlagSource("cod3_test_crash_logger") == rex::cvar::Source::kCommandLine;
    state.store(ok ? 1 : 2, std::memory_order_relaxed);
    if (ok)
      REXLOG_WARN("cod3: cod3_test_crash_logger: the game ends {} s from now (a test)", after);
    else
      REXLOG_WARN("cod3: cod3_test_crash_logger is a command-line test switch; not used");
  }
  if (state.load(std::memory_order_relaxed) != 1) return;
  if (Clock::now() - first < std::chrono::seconds(after)) return;
  uint8_t* base = GuestBase();
  if (!base) return;
  REXLOG_WARN("cod3: cod3_test_crash_logger: reading guest address 0x10 now; the game ends");
  rex::FlushLogging();
  volatile uint32_t value = cod3::guest::ReadU32(base, 0x10);
  (void)value;
}

// The crashing thread's guest side, when it is a guest thread: its handle and
// name (as the SDK's own line names it), lr / r1 / r3 as its register file
// last held them, and its guest call stack. Each frame's back chain is at
// 0(r1); the return address a function saved for its caller is 8 bytes below
// the caller's frame (mflr r12; stw r12,-8(r1) before stwu). The walk stays
// within the thread's stack as its PCR gives it (+0x70 top, +0x74 end).
std::string GuestThreadState() {
  using rex::system::XThread;
  if (!XThread::IsInThread()) return "not a guest thread";
  XThread* t = XThread::GetCurrentThread();
  auto* ts = rex::runtime::ThreadState::Get();
  uint8_t* base = GuestBase();
  if (!t || !ts || !ts->context() || !base) return "a guest thread; its state isn't available";
  const PPCContext& c = *ts->context();
  const auto lr = static_cast<uint32_t>(c.lr);
  const uint32_t r1 = c.r1.u32;
  std::string out = fmt::format("guest thread {:08X}{}{}, started at {:08X}: lr {}, r1 {:08X}, r3 {:08X}",
                                XThread::GetCurrentThreadHandle(), t->name().empty() ? "" : " ",
                                t->name(), t->creation_params()->start_address, GuestCode(lr), r1,
                                c.r3.u32);
  const uint32_t pcr = t->pcr_ptr();
  const uint32_t top = pcr ? cod3::guest::ReadU32(base, pcr + 0x70) : 0;
  const uint32_t end = pcr ? cod3::guest::ReadU32(base, pcr + 0x74) : 0;
  if (!(end && end < top && top - end <= (64u << 20) && r1 >= end && r1 < top))
    return out + "; r1 isn't inside its stack, so no guest call stack";
  // Game code a hook runs on a copy of the register file (guest::Call) keeps
  // its frames there, so this can stop short of the host stack below.
  out += "; guest call stack from that r1:";
  uint32_t sp = r1;
  for (int i = 0; i < 24; ++i) {
    if ((sp & 3) || sp + 4 > top) break;
    const uint32_t caller = cod3::guest::ReadU32(base, sp);
    if (caller <= sp || caller > top || (caller & 3) || caller - 8 < end) break;
    out += (i ? " <- " : " ") + GuestCode(cod3::guest::ReadU32(base, caller - 8));
    sp = caller;
  }
  return out;
}

#if defined(_WIN32)

// --- Last-chance crash logger (Windows) ------------------------------------

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
    case EXCEPTION_INVALID_DISPOSITION: return "invalid disposition";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun";
    case 0xC0000420: return "assertion failure";
    case 0xE06D7363: return "C++ exception (MSVC)";
    case 0x20474343: return "C++ exception (GCC/Clang)";
    default: return "other";
  }
}

// "module.dll+0x1234" (the file name only, no folder).
std::string ModuleOffset(uintptr_t address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module) ||
      !module) {
    return fmt::format("{:#x}", address);
  }
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
  const wchar_t* name = path;
  for (DWORD i = 0; i < length; ++i)
    if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
  char utf8[MAX_PATH * 3] = {};
  if (WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr) <= 0)
    utf8[0] = '?';
  return fmt::format("{}+{:#x}", utf8, address - reinterpret_cast<uintptr_t>(module));
}

// The faulting thread's host call stack, innermost first (x64 unwind data).
std::string HostStack(const CONTEXT& start) {
#if defined(_M_X64) || defined(__x86_64__)
  CONTEXT c = start;
  const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const auto low = reinterpret_cast<DWORD64>(tib->StackLimit);
  const auto high = reinterpret_cast<DWORD64>(tib->StackBase);
  std::string out;
  for (int i = 0; i < 16 && c.Rip; ++i) {
    if (i) out += " <- ";
    const std::string recompiled = RecompiledAt(uintptr_t(c.Rip), i > 0);
    out += recompiled.empty() ? ModuleOffset(c.Rip) : recompiled + " = " + ModuleOffset(c.Rip);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
    if (function) {
      void* handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, function, &c, &handler_data, &frame,
                       nullptr);
    } else {  // a leaf function: the return address is on top of the stack
      if (c.Rsp < low || c.Rsp + 8 > high) break;
      c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
      c.Rsp += 8;
    }
    if (c.Rsp < low || c.Rsp >= high) break;
  }
  return out;
#else
  (void)start;
  return "(not available on this CPU)";
#endif
}

LONG WINAPI LastChance(EXCEPTION_POINTERS* info) {
  if (!g_crash_logged.test_and_set()) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    std::string access;
    if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2) {
      const ULONG_PTR kind = rec->ExceptionInformation[0];
      const ULONG_PTR target = rec->ExceptionInformation[1];
      access = fmt::format(", {} {:#x}{}",
                           kind == 0 ? "reading" : kind == 1 ? "writing" : kind == 8 ? "executing"
                                                                                     : "using",
                           target,
                           target == ~ULONG_PTR(0)
                               ? " (a general protection fault, for example an SSE access to "
                                 "memory that isn't 16-byte aligned)"
                               : "");
    }
    const auto pc = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
    const std::string recompiled = RecompiledAt(pc, false);
    REXLOG_CRITICAL("cod3: crash: unhandled exception {:#010x} ({}) at {}{}{}, on {}; the process "
                    "ends now",
                    static_cast<uint32_t>(rec->ExceptionCode), ExceptionName(rec->ExceptionCode),
                    recompiled.empty() ? std::string() : recompiled + " = ", ModuleOffset(pc), access,
                    t_game_thread ? "the game's main thread" : "another thread");
    REXLOG_CRITICAL("cod3: crash:   {}", GuestThreadState());
    REXLOG_CRITICAL("cod3: crash:   game thread: last point passed: {}; {}", LastMark(),
                    GuestRegisters());
    REXLOG_CRITICAL("cod3: crash:   fibers: {}", cod3::ScriptThreadWatchLine());
    rex::FlushLogging();
    // Last, as walking a broken stack may itself fault.
    REXLOG_CRITICAL("cod3: crash:   host call stack: {}", HostStack(*info->ContextRecord));
    rex::FlushLogging();
  }
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// abort() (std::terminate, a failed check): say so before the C runtime ends
// the process.
void OnAbort(int) {
  if (!g_crash_logged.test_and_set()) {
    REXLOG_CRITICAL("cod3: crash: abort() was called on {}; the process ends now. Game thread: last "
                    "point passed: {}; {}",
                    t_game_thread ? "the game's main thread" : "another thread", LastMark(),
                    GuestRegisters());
    rex::FlushLogging();
  }
}

void InstallCrashLogger() {
  g_previous_filter = SetUnhandledExceptionFilter(&LastChance);
  std::signal(SIGABRT, &OnAbort);
}

#endif  // _WIN32

#if defined(__linux__)

// --- Last-chance crash logger (Linux) --------------------------------------
// The SDK's signal handler (rex/exception_handler.h) asks its handlers in the
// order they were installed; the guest memory system's (MMIO, watched pages)
// is installed while the runtime is built. This one is installed after it,
// so it only sees a fault nothing claimed: a real crash. It logs, and returns
// false: the SDK then hands the signal on and the process ends as before
// (until 2.12.9 such a crash left only the SDK's one line).

// "sub_8212C6A0+0x1f4 = cod3+0x1a2b3c", "memcpy+0x40 (libc.so.6)": the
// file name only, never a folder.
std::string HostFrame(uintptr_t pc, bool return_address) {
  std::string where = fmt::format("{:#x}", pc);
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    const char* file = std::strrchr(info.dli_fname, '/');
    file = file ? file + 1 : info.dli_fname;
    if (!*file) file = "cod3";
    if (info.dli_sname && info.dli_saddr) {
      std::string symbol = info.dli_sname;
      if (symbol.size() > 80) symbol = symbol.substr(0, 77) + "...";
      where = fmt::format("{}+{:#x} ({})", symbol, pc - reinterpret_cast<uintptr_t>(info.dli_saddr), file);
    } else {
      where = fmt::format("{}+{:#x}", file, pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
    }
  }
  const std::string recompiled = RecompiledAt(pc, return_address);
  return recompiled.empty() ? where : recompiled + " = " + where;
}

bool OnUnhandledFault(rex::arch::Exception* ex, void*) {
  using rex::arch::Exception;
  if (g_crash_logged.test_and_set()) return false;
  // If writing the report should hang (a lock the crashed thread held), the
  // process still ends: SIGALRM's default action.
  alarm(10);
  const auto pc = static_cast<uintptr_t>(ex->pc());
  std::string what = "an illegal instruction";
  if (ex->code() == Exception::Code::kAccessViolation) {
    const uintptr_t fault = static_cast<uintptr_t>(ex->fault_address());
    const auto op = ex->access_violation_operation();
    const uintptr_t base = reinterpret_cast<uintptr_t>(GuestBase());
    const bool guest = base && fault >= base && fault - base < (uintptr_t(1) << 32);
    what = fmt::format("an access violation ({} {:#x}{})",
                       op == Exception::AccessViolationOperation::kWrite  ? "writing"
                       : op == Exception::AccessViolationOperation::kRead ? "reading"
                                                                          : "at",
                       fault, guest ? fmt::format(", guest {:08X}", uint32_t(fault - base)) : "");
  }
  char name[32] = "?";
  pthread_getname_np(pthread_self(), name, sizeof(name));  // at most 15 characters
  REXLOG_CRITICAL("cod3: crash: {} at {}, on thread \"{}\" ({}); the process ends now", what,
                  HostFrame(pc, false), name,
                  t_game_thread ? "the game's main thread" : "not the game's main thread");
  REXLOG_CRITICAL("cod3: crash:   {}", GuestThreadState());
  REXLOG_CRITICAL("cod3: crash:   game thread: last point passed: {}; {}", LastMark(), GuestRegisters());
  REXLOG_CRITICAL("cod3: crash:   fibers: {}", cod3::ScriptThreadWatchLine());
  rex::FlushLogging();
  // Last, as walking a broken stack may itself fault. The first frames are
  // the signal handlers': the list starts at the faulting instruction.
  void* frames[48];
  const int count = backtrace(frames, 48);
  int first = 0;
  for (int i = 0; i < count; ++i) {
    if (reinterpret_cast<uintptr_t>(frames[i]) == pc) {
      first = i;
      break;
    }
  }
  std::string stack;
  for (int i = first, shown = 0; i < count && shown < 16; ++i, ++shown) {
    if (shown) stack += " <- ";
    stack += HostFrame(reinterpret_cast<uintptr_t>(frames[i]), i != first);
  }
  REXLOG_CRITICAL("cod3: crash:   host call stack: {}", stack);
  rex::FlushLogging();
  return false;
}

// Called from the watchdog thread once the runtime (and so the guest memory
// system's handler) exists.
void InstallCrashLogger() {
  void* warm_up[2];
  backtrace(warm_up, 2);  // loads the unwinder now, not in a signal handler
  rex::arch::ExceptionHandler::Install(&OnUnhandledFault, nullptr);
  // (No "crash" in this line: devrun.py counts lines with that word.)
  REXLOG_INFO("cod3: last-chance fault logger on: a fault nothing handles is written to this log "
              "with the game function and its guest and host call stacks");
}

// Waits (at most 60 s) for the runtime's memory to exist, then installs the
// logger; a crash in the first moments of start-up isn't logged.
void InstallCrashLoggerWhenReady() {
  for (int i = 0; i < 1200; ++i) {
    auto* ks = rex::system::kernel_state();
    if (ks && ks->memory()) {
      InstallCrashLogger();
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REXLOG_WARN("cod3: last-chance fault logger not installed: the runtime didn't start within 60 s");
}

#endif  // __linux__

}  // namespace

void Start() {
  static std::once_flag once;
  std::call_once(once, [] {
    BuildFunctionTables();
#if defined(_WIN32)
    InstallCrashLogger();
#endif
    // Never joined: the process ends with std::_Exit / TerminateProcess.
    std::thread(&Watch).detach();
  });
}

void Mark(const char* where) {
  if (!t_game_thread) return;
  g_where.store(where, std::memory_order_relaxed);
  g_marks.store(g_marks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void MarkFrame() {
  t_game_thread = true;
  Mark("Com_Frame");
  TestCrashIfAsked();  // [crash-logger] a test switch, off
}

void SetGameContext(const PPCContext* ctx) {
  if (t_game_thread) g_ctx.store(ctx, std::memory_order_release);
}

uint64_t MarkCount() {  // [hw-scaling]
  return g_marks.load(std::memory_order_relaxed);
}

std::string GuestCodeName(uint32_t addr) { return GuestCode(addr); }  // [heap-check]

}  // namespace cod3::watchdog

// --- Marks on the game thread's long-running paths ---------------------------
// A level load runs inside one Com_Frame, and a film plays in a loop of its
// own; these keep the watchdog quiet through them.

// One step of the level loader (the load's pump loop, 0x82463958, and others).
REX_EXTERN(__imp__sub_82462FB0);
REX_HOOK_RAW(sub_82462FB0) {
  cod3::watchdog::SetGameContext(&ctx);
  cod3::watchdog::Mark("the level loader");
  __imp__sub_82462FB0(ctx, base);
}

// A screen update (frames, loading screens, films; then 0x82130980 presents).
REX_EXTERN(__imp__sub_82525190);
REX_HOOK_RAW(sub_82525190) {
  cod3::watchdog::SetGameContext(&ctx);
  cod3::watchdog::Mark("a screen update");
  __imp__sub_82525190(ctx, base);
}
