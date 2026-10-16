// cod3 - hang watchdog and last-chance crash logger (see watchdog.h).

#include "game/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/script_threads.h"

#if defined(_WIN32)
#include <csignal>

#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(cod3_watchdog_seconds, 10, "cod3",
                     "Log what the game's main thread was doing when it makes no progress for "
                     "this many seconds (0 = off)")
    .range(0, 3600);

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
    {0x823494F8, "NtWaitForSingleObjectEx in sub_823494C0 (WaitForSingleObjectEx)"},
    {0x823444FC, "NtWaitForSingleObjectEx in sub_82344420 (a file write finishing)"},
    {0x82344654, "NtWaitForSingleObjectEx in sub_82344558 (a file read finishing)"},
    {0x82344494, "NtWriteFile in sub_82344420"},
    {0x823444E0, "NtWriteFile in sub_82344420"},
    {0x82345630, "NtWriteFile in sub_823455E8"},
    {0x823445D0, "NtReadFile in sub_82344558"},
    {0x82344638, "NtReadFile in sub_82344558"},
    {0x823455B8, "NtReadFile in sub_82345570"},
    {0x82349740, "KeDelayExecutionThread in sub_823496E0 (Sleep)"},
    {0x82345050, "NtYieldExecution in sub_82345040"},
    {0x822F4DB8, "VdSwap in sub_822F4C28 (presenting a frame)"},
    {0x822C0FE8, "KeWaitForSingleObject in sub_822C0F90"},
    {0x822C10A8, "KeWaitForSingleObject in sub_822C1058"},
    {0x822C10D8, "KeWaitForSingleObject in sub_822C1058"},
    {0x822C11AC, "KeWaitForMultipleObjects in sub_822C1108"},
    {0x8230258C, "KeWaitForSingleObject in sub_823024D8"},
    {0x82302620, "KeWaitForSingleObject in sub_823025B0"},
    {0x82302D1C, "KeWaitForSingleObject in sub_82302C80"},
    {0x821960B8, "XMsgInProcessCall in sub_82196070"},
    {0x82195AC4, "XMsgInProcessCall in sub_82195A90"},
    {0x82195BE4, "XMsgInProcessCall in sub_82195BA8"},
    {0x8219627C, "XMsgInProcessCall in sub_82196188"},
    {0x82195B60, "XMsgStartIORequestEx in sub_82195B00"},
    {0x82343B14, "XMsgStartIORequest in sub_82343AD8"},
    {0x82343BA8, "XMsgStartIORequest in sub_82343B58"},
    {0x82343E9C, "XexLoadImage in sub_82343E80 (loading a level's code)"},
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
}

// The watchdog thread: once a second, has the game thread marked anything?
void Watch() {
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

#if defined(_WIN32)

// --- Last-chance crash logger (Windows) ------------------------------------

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
std::atomic_flag g_crash_logged;  // starts clear (C++20)

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
    out += ModuleOffset(c.Rip);
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
    REXLOG_CRITICAL("cod3: crash: unhandled exception {:#010x} ({}) at {}{}, on {}; the process "
                    "ends now",
                    static_cast<uint32_t>(rec->ExceptionCode), ExceptionName(rec->ExceptionCode),
                    ModuleOffset(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)), access,
                    t_game_thread ? "the game's main thread" : "another thread");
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

}  // namespace

void Start() {
  static std::once_flag once;
  std::call_once(once, [] {
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
}

void SetGameContext(const PPCContext* ctx) {
  if (t_game_thread) g_ctx.store(ctx, std::memory_order_release);
}

uint64_t MarkCount() {  // [hw-scaling]
  return g_marks.load(std::memory_order_relaxed);
}

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
