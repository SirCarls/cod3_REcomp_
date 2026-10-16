// cod3 - restore the game's console output.
//
// Retail CoD3 compiled Com_Printf and Com_DPrintf down to a bare `blr`, but
// every call site still passes the format string and its arguments in the
// usual registers / stack slots. Hooking the empty functions and formatting
// those arguments brings back all engine output (cmdlist, cvar queries,
// "Unknown command", script warnings, ...) for the in-game console.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/dbg.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/format.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game/addresses.h"
#include "game/engine.h"
#include "game/guest.h"
#include "ui/game_console.h"

namespace {

// default.xex in guest memory (generated/default/cod3_pch.h: REX_IMAGE_BASE,
// REX_IMAGE_SIZE).
constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kImageSize = 0xC30000;
constexpr uint32_t kCodeEnd = 0x820A0000 + 0x4FB1B8;  // REX_CODE_BASE + REX_CODE_SIZE
constexpr uint32_t kPageSize = 0x1000;
constexpr size_t kMaxFormatChars = 2048;

// Whether `addr` is in the game's read-only data (its own string constants),
// as the SDK's loader protected the image's sections. A format anywhere else
// (va()'s buffer, the heap, the stack) was built at run time and may hold
// player text.
bool InReadOnlyImage(uint32_t addr) {
  static const std::vector<bool> read_only = [] {
    std::vector<bool> pages(kImageSize / kPageSize, false);
    auto* kernel_state = REX_KERNEL_STATE();
    auto* memory = kernel_state ? kernel_state->memory() : nullptr;
    size_t found = 0;
    for (size_t i = 0; memory && i < pages.size(); ++i) {
      const uint32_t page = kImageBase + uint32_t(i) * kPageSize;
      auto* heap = memory->LookupHeap(page);
      uint32_t protect = 0;
      pages[i] = heap && heap->QueryProtect(page, &protect) &&
                 (protect & rex::memory::kMemoryProtectRead) &&
                 !(protect & rex::memory::kMemoryProtectWrite);
      found += pages[i];
    }
    if (!found) {
      // No protection information: the read-only data and code before the
      // end of the code (va()'s buffer and the other writable data are after).
      for (size_t i = 0; i < pages.size(); ++i) pages[i] = kImageBase + i * kPageSize < kCodeEnd;
      REXLOG_WARN("cod3: no read-only pages found in the game image; game print formats are taken "
                  "from below 0x{:08X}", kCodeEnd);
    } else {
      REXLOG_INFO("cod3: game print formats: {} KB of read-only image data", found * kPageSize / 1024);
    }
    return pages;
  }();
  if (addr < kImageBase || addr - kImageBase >= kImageSize) return false;
  return read_only[(addr - kImageBase) / kPageSize];
}

// The format with each %n (it would write the count to a guest address taken
// from the arguments) turned into %.0s, which takes the same argument and
// prints nothing. Retail CoD3 never formatted (its print functions are
// empty), so nothing the game relies on is lost.
std::string WithoutStores(const std::string& fmt) {
  std::string out;
  out.reserve(fmt.size());
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    size_t j = i + 1;
    while (j < fmt.size() && std::strchr("-+ #0123456789.*hlLwIjzt", fmt[j])) ++j;
    if (j < fmt.size() && fmt[j] == 'n') {
      out += "%.0s";
    } else {
      out.append(fmt, i, std::min(j + 1, fmt.size()) - i);
    }
    i = j;
  }
  return out;
}

// The text a guest print call means. The game's own constant formats are
// formatted with their arguments; anything else is printed as it is: in this
// game such a "format" is already finished text (Com_Error(code, va(...)) in
// Cvar_Set2 / Cvar_Get carries the dvar name the player typed), and
// formatting it again would let "%n" or "%s" in a dvar name write or read
// guest memory.
std::string FormatGuest(PPCContext& ctx, uint8_t* base, uint32_t fmt_addr, int first_arg_index) {
  if (fmt_addr == 0) return {};
  const std::string fmt = cod3::guest::ReadString(base, fmt_addr, kMaxFormatChars);
  if (!InReadOnlyImage(fmt_addr)) return fmt;
  using namespace rex::system::format;
  const std::string safe = WithoutStores(fmt);
  StackArgList args(ctx, base, first_arg_index);
  StringFormatData data(reinterpret_cast<const uint8_t*>(safe.c_str()));
  format_core(base, data, args, false);
  return data.str();
}

// The log copy of a line that repeats the one before it within a second is
// only counted. In big fights the game prints "CM_AreaEntities: MAXCOUNT"
// several times a frame (about 300 a second), and each log line is written
// to disk from the game's main thread. The in-game console still gets every
// line.
void LogGameLine(const std::string& line) {
  using Clock = std::chrono::steady_clock;
  static std::mutex mutex;
  static std::string last;
  static Clock::time_point last_logged{};
  static uint32_t repeats = 0;
  const auto now = Clock::now();
  std::lock_guard lock(mutex);
  if (line == last && now - last_logged < std::chrono::seconds(1)) {
    ++repeats;
    return;
  }
  if (repeats) REXLOG_INFO("[game] (previous line repeated {} more times)", repeats);
  repeats = 0;
  last = line;
  last_logged = now;
  REXLOG_INFO("[game] {}", line);
}

void Emit(cod3::ui::ConsoleChannel channel, std::string text) {
  if (text.empty()) return;
  cod3::ui::GameConsole::Get().Print(channel, text);
  // Mirror to the log file so boot problems can be diagnosed without the UI.
  std::string line = text;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (!line.empty()) LogGameLine(line);
}

}  // namespace

// void Com_Printf(const char* fmt, ...)
REX_HOOK_RAW(Com_Printf) {
  Emit(cod3::ui::ConsoleChannel::kGame, FormatGuest(ctx, base, ctx.r3.u32, 1));
}

// void Com_DPrintf(const char* fmt, ...) - only shown with `developer 1`, like the PC games.
REX_HOOK_RAW(Com_DPrintf) {
  uint32_t dev = cod3::Engine::Get().developer_cvar();
  bool show = dev && cod3::guest::ReadS32(base, dev + cod3::addr::cvar::kInteger) != 0;
  if (!show) return;
  Emit(cod3::ui::ConsoleChannel::kDeveloper, FormatGuest(ctx, base, ctx.r3.u32, 1));
}

// void Com_Error(int code, const char* fmt, ...)
REX_EXTERN(__imp__Com_Error);
REX_HOOK_RAW(Com_Error) {
  std::string msg = FormatGuest(ctx, base, ctx.r4.u32, 2);
  // Error strings carry a leading 0x15 control byte in this build.
  if (!msg.empty() && static_cast<unsigned char>(msg[0]) < 0x20) msg.erase(0, 1);
  REXLOG_ERROR("Com_Error({}): {}", ctx.r3.s32, msg);
  Emit(cod3::ui::ConsoleChannel::kError, "^1ERROR: " + msg);
  // Retail Com_Error is only DbgBreakPoint (0x82343910), and its callers
  // carry on after it. On PC a breakpoint without a debugger ends the program
  // (Linux: the second SIGTRAP; Windows: an unhandled breakpoint), so a dvar
  // name with a backslash typed in the console or read from a config closed
  // the game. Stop only for a debugger.
  if (rex::debug::IsDebuggerAttached()) __imp__Com_Error(ctx, base);
}
