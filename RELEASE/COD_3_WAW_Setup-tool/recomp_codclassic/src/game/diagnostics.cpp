// codclassic - the game's errors in the port's log. [acct2-uo]
//
// Com_Error(code r3, format r4, arguments r5...) is where every Quake III error goes: a drop to the menu
// (a missing map, a script error, a disconnect) or a fatal one. CoD Classic writes the message to its own
// console only; this hook writes it to the port's log first, so a run's log says why the game left a level.
// The message is rebuilt from the format and the first six integer arguments (r5-r10): %s, %d/%i/%u, %x/%X
// and %c are filled in, a floating-point argument (passed in a floating-point register) shows as "?".
// Com_Error then longjmps to the frame loop (setjmp/longjmp are the host's, config/default.toml).
//   campaign 0x8207D580, MP 0x82086C20 (each found as the one function that longjmps to the abort frame).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#if defined(CODCLASSIC_MP)
#define CODCLASSIC_COM_ERROR sub_82086C20
#else
#define CODCLASSIC_COM_ERROR sub_8207D580
#endif
#define CODCLASSIC_DIAG_IMP_(name) __imp__##name
#define CODCLASSIC_DIAG_IMP(name) CODCLASSIC_DIAG_IMP_(name)

REX_EXTERN(CODCLASSIC_DIAG_IMP(CODCLASSIC_COM_ERROR));  // Com_Error(code r3, format r4, ...)

namespace {

constexpr size_t kMaxText = 512;

// A guest string, at most kMaxText bytes; empty for a null or out-of-range pointer.
std::string GuestString(const uint8_t* base, uint32_t address) {
  if (address < 0x10000u) return {};
  const char* text = reinterpret_cast<const char*>(base + address);
  return std::string(text, strnlen(text, kMaxText));
}

std::string RebuildMessage(const PPCContext& ctx, const uint8_t* base, const std::string& format) {
  const uint64_t arguments[] = {ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  size_t next = 0;
  std::string out;
  for (size_t i = 0; i < format.size() && out.size() < kMaxText; ++i) {
    if (format[i] != '%' || i + 1 >= format.size()) {
      out += format[i];
      continue;
    }
    // Skip flags, width, precision and length ("%-8.2f", "%ld").
    size_t j = i + 1;
    while (j < format.size() && std::strchr("-+ #0123456789.lhz", format[j])) ++j;
    if (j >= format.size()) break;
    const char kind = format[j];
    i = j;
    if (kind == '%') {
      out += '%';
      continue;
    }
    if (kind == 'f' || kind == 'g' || kind == 'e') {
      out += '?';
      continue;
    }
    if (next >= sizeof(arguments) / sizeof(arguments[0])) {
      out += "...";
      break;
    }
    const uint64_t value = arguments[next++];
    switch (kind) {
      case 's': out += GuestString(base, static_cast<uint32_t>(value)); break;
      case 'd': case 'i': out += std::to_string(static_cast<int32_t>(value)); break;
      case 'u': out += std::to_string(static_cast<uint32_t>(value)); break;
      case 'c': out += static_cast<char>(value & 0xFF); break;
      case 'x': case 'X': case 'p': {
        char hex[16];
        std::snprintf(hex, sizeof(hex), kind == 'X' ? "%X" : "%x", static_cast<uint32_t>(value));
        out += hex;
        break;
      }
      default: out += '%'; out += kind; break;
    }
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

// Every string lives only in here: Com_Error longjmps past the hook's frame, which then never returns.
void LogError(const PPCContext& ctx, const uint8_t* base) {
  const std::string format = GuestString(base, ctx.r4.u32);
  REXLOG_WARN("codclassic: game error (Com_Error code {}): \"{}\"", static_cast<int32_t>(ctx.r3.u32),
              RebuildMessage(ctx, base, format));
}

}  // namespace

REX_HOOK_RAW(CODCLASSIC_COM_ERROR) {
  LogError(ctx, base);
  CODCLASSIC_DIAG_IMP(CODCLASSIC_COM_ERROR)(ctx, base);
}
