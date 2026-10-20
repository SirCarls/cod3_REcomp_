// cod3 - [native-printf] the C runtime's formatted output engine as native
// code: _output_l, the function under sprintf, _vsnprintf, vsprintf,
// _snprintf, printf and fprintf.
//
// This is the Xbox 360 C runtime's own PowerPC function (737 instructions),
// translated by the recompiler; sprintf alone has 90 call sites in the
// campaign and Com_sprintf 27, and the translated code writes each character
// through a function call. Here it is a table-driven state machine over the
// format string, the same as the game's code:
//   - Each format character's class and the next state come from the game's
//     own table (__lookuptable, read from its memory as the code reads it);
//     the eight states do what the game's code does, including the quirks:
//     '*' widths and precisions read as 32-bit values (a negative width means
//     '-'), "I" followed by anything but 64, 32 or an integer conversion is
//     printed as a character, 'l' 'l' is a 64-bit value, the 32-bit wrapping
//     of width and precision digits, precisions above 512 cut to 512, "(null)"
//     for a null %s, "0x" left out for a zero %#x, a forced '0' for %#o.
//   - Arguments come from the va_list in 8-byte slots (a 32-bit value is the
//     slot's low word, a 64-bit one the whole slot), as the game's code reads
//     them.
//   - Characters go to a string stream (the FILE that sprintf and _vsnprintf
//     make on their stack): written at _ptr, _ptr and _cnt moved on, or only
//     counted when the stream has no buffer.
// A call runs natively only where that provably does what the game's code
// does; everything else runs the game's code, decided before anything is
// written:
//   - floating point conversions (%e %f %g %a and capitals: the game's own
//     conversion code), wide characters and strings (%C %S %lc %ls %wc %ws),
//     %n, %Z, conversions the game's code has no case for, multibyte lead
//     bytes, file streams (printf, fprintf) and stdout/stderr;
//   - output that wouldn't fit the stream's count (the game's code then sets
//     errno and the stream's error flag) or is longer than kMaxOut;
//   - a format, argument slot or string that isn't plain guest memory (see
//     crt_native.cpp), and output that overlaps anything the call reads (the
//     format, the argument slots, the strings, the FILE, the game's tables)
//     or the stack below the caller (the game's code would build its frame
//     there).
// The native version reads everything first and writes the finished output at
// the end, so a call it gives up on goes to the game's code untouched.
//
// Before the first native call, a check runs the game's code and the native
// version on thousands of random formats (flags, widths, precisions, sizes,
// '*' arguments, extreme integers, null strings, unsupported conversions,
// random format soup, small and missing buffers, overlapping output) in two
// identical blocks of scratch guest memory and compares every byte and the
// returned value. If one differs, the native version stays off for the
// session (logged). The game's first calls are compared too, then one call in
// kSpotEvery per thread. cod3_native_printf (on) switches it off at any time;
// cod3_native_printf_check_all (off) compares every call (tests);
// cod3_native_printf_test N runs N more random formats on the next call;
// cod3_native_printf_report writes the counts to the log.
//
// Both programs: cod3mp has the same function (the same instructions; only
// the tables' addresses differ, COD3_ADDR) and tools/mp_aliases.py points the
// hook at its copy (mp/config/campaign_map.tsv).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "game/program.h"

REXCVAR_DEFINE_BOOL(cod3_native_printf, true, "cod3/Performance",
                    "The C runtime's formatted output (sprintf, _vsnprintf and the like) runs as "
                    "native code where that does exactly what the game's own code does (checked "
                    "at start; off: the translated Xbox 360 code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_native_printf_check_all, false, "cod3/Performance",
                    "Testing: every native sprintf-style call runs both ways and the results are "
                    "compared (slower)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82353B58);  // _output_l (cod3mp sub_82323758)

namespace {

// ---------------------------------------------------------------------------
// Guest memory, the way the generated code reaches it (as in crt_native.cpp)
// ---------------------------------------------------------------------------

#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && defined(REX_ARCH_ARM64))
constexpr uint32_t kHighViewOffset = 0x1000;
#else
constexpr uint32_t kHighViewOffset = 0;
#endif
constexpr uint32_t kHighView = 0xE0000000u;
constexpr uint32_t kMmioBegin = 0x7F000000u;
constexpr uint32_t kMmioEnd = 0x80000000u;

std::atomic<int> g_high_view{-1};  // -1 not probed yet, 0 no, 1 yes

bool HighViewOk(uint8_t* base) {
  int v = g_high_view.load(std::memory_order_relaxed);
  if (v < 0) {
    auto* ks = rex::system::kernel_state();
    auto* mem = ks ? ks->memory() : nullptr;
    const uint32_t probe = 0xE0010000u;
    v = (mem && mem->TranslateVirtual<uint8_t*>(probe) == base + probe + kHighViewOffset) ? 1 : 0;
    g_high_view.store(v, std::memory_order_relaxed);
  }
  return v == 1;
}

inline uint8_t* Host(uint8_t* base, uint32_t addr) {
  return base + addr + (addr >= kHighView ? kHighViewOffset : 0u);
}

// [begin, begin + size) is plain guest memory (64-bit so nothing wraps here).
bool Plain(uint8_t* base, uint64_t begin, uint64_t size) {
  const uint64_t end = begin + size;
  if (end > 0x100000000ull) return false;
  if (begin < kMmioEnd && end > kMmioBegin) return false;
  if (end > kHighView) {
    if (begin < kHighView) return false;
    if (!HighViewOk(base)) return false;
  }
  return true;
}

// How many bytes from addr on are plain guest memory reached through one host
// pointer.
uint64_t PlainRun(uint8_t* base, uint32_t addr) {
  if (addr < kMmioBegin) return uint64_t(kMmioBegin) - addr;
  if (addr < kMmioEnd) return 0;
  if (addr < kHighView) return uint64_t(kHighView) - addr;
  return HighViewOk(base) ? 0x100000000ull - addr : 0;
}

inline uint32_t LdW(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline uint16_t LdH(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline uint64_t LdD(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
inline void StW(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void StD(uint8_t* p, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}

inline bool Overlap(uint64_t a, uint64_t na, uint64_t b, uint64_t nb) {
  return a < b + nb && b < a + na;
}

// ---------------------------------------------------------------------------
// The game's tables (read-only data of the program; cod3mp's sit elsewhere)
// ---------------------------------------------------------------------------

// __lookuptable: byte ch - 32 (for ' ' to 'x') has the character's class in
// its low nibble; byte class * 8 + state has the next state in its high one.
constexpr uint32_t kLookup = COD3_ADDR(0x8205BCB8, 0x8205A740);
constexpr uint32_t kLookupSize = 128;
// The address of "(null)", printed for a null %s.
constexpr uint32_t kNullStringPtr = COD3_ADDR(0x8205BCB0, 0x8205A738);
// The locale data's address; +200 holds the ctype table _isleadbyte reads.
constexpr uint32_t kLocalePtr = COD3_ADDR(0x825C7048, 0x825CC558);
// _iob: stdout is +32, stderr +64 (the game's code prints those to the debug
// output through a 512-byte buffer).
constexpr uint32_t kIob = COD3_ADDR(0x825C6B50, 0x825CC060);

// The FILE fields the code uses.
constexpr uint32_t kFilePtr = 0, kFileCnt = 4, kFileBase = 8, kFileFlag = 12;
constexpr uint32_t kIoStrg = 0x40;  // _IOSTRG: a string stream

// The flags the code keeps per conversion (r27 in the game's code).
constexpr uint32_t kFlSign = 0x1, kFlSpace = 0x2, kFlLeft = 0x4, kFlLeadZero = 0x8,
                   kFlLong = 0x10, kFlShort = 0x20, kFlSigned = 0x40, kFlAlternate = 0x80,
                   kFlNegative = 0x100, kFlForceOctal = 0x200, kFlWideChar = 0x800,
                   kFlLongLong = 0x1000, kFlI64 = 0x8000;

constexpr uint32_t kMaxOut = 64 * 1024;     // longer output goes to the game's code
constexpr uint32_t kMaxFormat = 64 * 1024;  // so is a longer format
constexpr int kMaxReads = 48;               // ranges the call reads (strings and the rest)
constexpr uint32_t kGuestFrame = 0x1000;    // the stack the game's code would use below r1

// Why a call went to the game's code (or kNative).
enum Why : int {
  kNative = 0,
  kFileStream,  // not a string stream: printf, fprintf
  kStdStream,   // stdout or stderr
  kNullArg,     // no stream or no format: the game's code reports the invalid parameter
  kFloat,       // %e %f %g %a and capitals
  kWide,        // wide characters or strings
  kOtherConv,   // %n, %Z, a conversion with no case, a lead byte, a state past the table
  kMemory,      // not plain guest memory, or the output overlaps what the call reads
  kTooLong,     // more than kMaxOut characters, a format past kMaxFormat, too many strings
  kFull,        // the output doesn't fit the stream's count
  kWhyCount
};
constexpr const char* kWhyName[kWhyCount] = {
    "native",           "file streams",          "stdout/stderr", "no stream or format",
    "floating point",   "wide characters",       "other conversions", "memory",
    "too long",         "not fitting the buffer"};

// One call's result before it is written: the stream's state, the output and
// the ranges the call read (for the overlap rule).
struct Out {
  uint32_t stream = 0;
  uint32_t ptr = 0;  // _ptr
  uint32_t cnt = 0;  // _cnt (a signed count in the game's code)
  bool count_only = false;
  uint32_t n = 0;  // characters (the value returned)
  uint8_t* buf = nullptr;
  uint32_t fmt = 0;
  uint32_t read_begin[kMaxReads];
  uint64_t read_size[kMaxReads];
  int reads = 0;
};

uint8_t* OutBuffer() {
  thread_local std::unique_ptr<uint8_t[]> buf;
  if (!buf) buf.reset(new uint8_t[kMaxOut + 16]);
  return buf.get();
}

inline bool AddRead(Out* o, uint32_t begin, uint64_t size) {
  if (size == 0) return true;
  if (o->reads == kMaxReads) return false;
  o->read_begin[o->reads] = begin;
  o->read_size[o->reads] = size;
  ++o->reads;
  return true;
}

// The game's count loop for %s: at most `limit` bytes (unsigned, as its 32-bit
// down-counter runs), up to the first NUL. *read: the bytes it reads.
Why StrLen(uint8_t* base, uint32_t s, uint32_t limit, uint32_t* len, uint32_t* read) {
  const uint64_t cap = std::min<uint64_t>(limit, uint64_t(kMaxOut) + 1);
  const uint64_t run = PlainRun(base, s);
  const uint64_t span = std::min(run, cap);
  const uint8_t* p = Host(base, s);
  const void* z = span ? std::memchr(p, 0, size_t(span)) : nullptr;
  if (z) {
    *len = uint32_t(static_cast<const uint8_t*>(z) - p);
    *read = *len + 1;
    return kNative;
  }
  if (span < cap) return kMemory;  // plain memory ends first
  if (cap < limit) return kTooLong;
  *len = limit;
  *read = limit;
  return kNative;
}

// The next argument slot: 8-byte aligned, 8 bytes on (32-bit wrapping, as the
// game's code computes it).
// Slots past 4 GB (wrapping) go to the game's code.
inline const uint8_t* Slot(uint8_t* base, uint32_t* ap, uint32_t* first, uint32_t* end) {
  const uint32_t a = (*ap + 7u) & ~7u;
  *ap = a + 8u;
  if (a > 0xFFFFFFF0u || !Plain(base, a, 8)) return nullptr;
  if (*end == 0) *first = a;
  *end = a + 8u;
  return Host(base, a);
}

// The output as it is made (in o->buf).
struct Writer {
  uint8_t* buf;
  uint32_t n = 0;
  bool Put(uint8_t c) {
    if (n >= kMaxOut) return false;
    buf[n++] = c;
    return true;
  }
  bool Pad(uint8_t c, int32_t k) {  // the game's code: k times, when k > 0
    if (k <= 0) return true;
    if (uint64_t(n) + uint32_t(k) > kMaxOut) return false;
    std::memset(buf + n, c, uint32_t(k));
    n += uint32_t(k);
    return true;
  }
  bool Str(const uint8_t* s, int32_t k) {  // write_string: k bytes, when k > 0
    if (k <= 0) return true;
    if (uint64_t(n) + uint32_t(k) > kMaxOut) return false;
    std::memcpy(buf + n, s, uint32_t(k));
    n += uint32_t(k);
    return true;
  }
};

// _output_l(r3 = stream, r4 = format, r5 = locale (unused by this code),
// r6 = va_list), as the game's code does it, into o (nothing written to guest
// memory). r1: the caller's stack pointer.
Why Format(uint8_t* base, uint32_t r1, uint32_t stream, uint32_t fmt, uint32_t argp, Out* o) {
  o->reads = 0;
  o->n = 0;
  o->fmt = fmt;
  o->stream = stream;
  if (!o->buf) o->buf = OutBuffer();
  if (stream == 0 || fmt == 0) return kNullArg;
  if (!Plain(base, stream, 16)) return kMemory;
  const uint8_t* fs = Host(base, stream);
  if (!(LdW(fs + kFileFlag) & kIoStrg)) return kFileStream;
  if (stream == kIob + 32 || stream == kIob + 64) return kStdStream;
  o->ptr = LdW(fs + kFilePtr);
  o->cnt = LdW(fs + kFileCnt);
  o->count_only = LdW(fs + kFileBase) == 0;  // write_char and write_string only count then
  AddRead(o, stream, 16);

  uint32_t flen = 0, fread = 0;
  if (const Why w = StrLen(base, fmt, kMaxFormat, &flen, &fread); w != kNative) return w;
  if (fread == flen) return kTooLong;  // no NUL within kMaxFormat
  AddRead(o, fmt, fread);
  if (!Plain(base, kLookup, kLookupSize)) return kMemory;
  AddRead(o, kLookup, kLookupSize);
  const uint8_t* lk = Host(base, kLookup);
  const uint8_t* f = Host(base, fmt);
  const uint8_t* ctab = nullptr;  // the ctype table, at the first plain character

  Writer w{o->buf};
  uint32_t ap = argp, args_first = 0, args_end = 0;
  // The game's code's per-conversion state (registers and stack slots there),
  // kept across conversions as it keeps them.
  uint32_t flags = 0, state = 0;
  uint32_t precision = 0, width = 0;  // 32-bit, wrapping; compared as signed
  uint32_t prefixlen = 0, hexadd = 0;
  bool no_output = false;
  uint8_t prefix[2] = {0, 0};
  int32_t textlen = 0;
  const uint8_t* text = nullptr;  // garbage in the game's code until a conversion sets it
  uint8_t numbuf[520];
  constexpr int kNumEnd = 515;  // the last digit's place (r1 + 655 there)
  uint8_t charbuf[1] = {0};

  // A character in the plain state: a lead byte goes to the game's code (it
  // copies the next byte raw, or reports an invalid parameter at the end).
  auto plain_char = [&](int ch) -> Why {
    if (!ctab) {
      if (!Plain(base, kLocalePtr, 4)) return kMemory;
      const uint32_t loc = LdW(Host(base, kLocalePtr));
      if (!Plain(base, uint64_t(loc) + 200, 4)) return kMemory;
      const uint32_t table = LdW(Host(base, loc + 200));
      if (!Plain(base, table, 512)) return kMemory;
      if (!AddRead(o, kLocalePtr, 4) || !AddRead(o, loc + 200, 4) || !AddRead(o, table, 512))
        return kTooLong;
      ctab = Host(base, table);
    }
    if (LdH(ctab + 2 * uint32_t(ch & 0xFF)) & 0x8000) return kOtherConv;
    return w.Put(uint8_t(ch)) ? kNative : kTooLong;
  };

  uint32_t i = 0;  // the next format byte (r17 there)
  int ch = int8_t(f[0]);
  while (ch != 0) {
    ++i;
    const uint32_t cls = (ch >= 32 && ch <= 120) ? (lk[ch - 32] & 0xFu) : 0u;
    state = uint32_t(int32_t(int8_t(lk[cls * 8 + state])) >> 4);
    if (state > 7) return kOtherConv;  // the game's code skips it and indexes past the table
    bool output = false;
    switch (state) {
      case 0:  // plain character
        if (const Why r = plain_char(ch); r != kNative) return r;
        break;
      case 1:  // '%'
        flags = 0;
        precision = 0xFFFFFFFFu;
        no_output = false;
        width = 0;
        prefixlen = 0;
        break;
      case 2:  // flag
        switch (ch) {
          case ' ': flags |= kFlSpace; break;
          case '#': flags |= kFlAlternate; break;
          case '+': flags |= kFlSign; break;
          case '-': flags |= kFlLeft; break;
          case '0': flags |= kFlLeadZero; break;
          default: break;
        }
        break;
      case 3:  // width
        if (ch == '*') {
          const uint8_t* s = Slot(base, &ap, &args_first, &args_end);
          if (!s) return kMemory;
          const uint32_t v = LdW(s + 4);
          width = v;
          if (int32_t(v) < 0) {
            flags |= kFlLeft;
            width = 0u - v;
          }
        } else {
          width = width * 10u + uint32_t(ch) - 48u;
        }
        break;
      case 4:  // '.'
        precision = 0;
        break;
      case 5:  // precision
        if (ch == '*') {
          const uint8_t* s = Slot(base, &ap, &args_first, &args_end);
          if (!s) return kMemory;
          precision = LdW(s + 4);
          if (int32_t(precision) < 0) precision = 0xFFFFFFFFu;
        } else {
          precision = precision * 10u + uint32_t(ch) - 48u;
        }
        break;
      case 6:  // size
        switch (ch) {
          case 'I': {
            const int c0 = int8_t(f[i]);
            if (c0 == '6' && f[i + 1] == '4') {
              i += 2;
              flags |= kFlI64;
            } else if (c0 == '3' && f[i + 1] == '2') {
              i += 2;
              flags &= ~kFlI64;
            } else if (c0 == 'd' || c0 == 'i' || c0 == 'o' || c0 == 'u' || c0 == 'x' ||
                       c0 == 'X') {
              // an integer conversion follows: nothing changes
            } else {
              state = 0;  // printed as a plain character
              if (const Why r = plain_char(ch); r != kNative) return r;
            }
            break;
          }
          case 'h': flags |= kFlShort; break;
          case 'l':
            if (f[i] == 'l') {
              ++i;
              flags |= kFlLongLong;
            } else {
              flags |= kFlLong;
            }
            break;
          case 'w': flags |= kFlWideChar; break;
          default: break;
        }
        break;
      case 7: {  // the conversion
        uint32_t radix = 0;
        switch (ch) {
          case 'C':
            if (!(flags & (kFlShort | kFlLong | kFlWideChar))) flags |= kFlWideChar;
            [[fallthrough]];
          case 'c': {
            const uint8_t* s = Slot(base, &ap, &args_first, &args_end);
            if (!s) return kMemory;
            if (flags & (kFlLong | kFlWideChar)) return kWide;
            charbuf[0] = uint8_t(LdW(s + 4));
            text = charbuf;
            textlen = 1;
            output = true;
            break;
          }
          case 'S':
            if (!(flags & (kFlShort | kFlLong | kFlWideChar))) flags |= kFlWideChar;
            [[fallthrough]];
          case 's': {
            const uint32_t limit = int32_t(precision) == -1 ? 0x7FFFFFFFu : precision;
            const uint8_t* s = Slot(base, &ap, &args_first, &args_end);
            if (!s) return kMemory;
            if (flags & (kFlLong | kFlWideChar)) return kWide;
            uint32_t str = LdW(s + 4);
            if (str == 0) {
              if (!Plain(base, kNullStringPtr, 4)) return kMemory;
              if (!AddRead(o, kNullStringPtr, 4)) return kTooLong;
              str = LdW(Host(base, kNullStringPtr));
            }
            uint32_t len = 0, read = 0;
            if (const Why r = StrLen(base, str, limit, &len, &read); r != kNative) return r;
            if (!AddRead(o, str, read)) return kTooLong;
            text = Host(base, str);
            textlen = int32_t(len);
            output = true;
            break;
          }
          case 'd':
          case 'i':
            flags |= kFlSigned;
            radix = 10;
            break;
          case 'u': radix = 10; break;
          case 'p':
            precision = 8;
            [[fallthrough]];
          case 'X':
          case 'x':
            hexadd = ch == 'x' ? 39 : 7;
            radix = 16;
            if (flags & kFlAlternate) {
              prefix[0] = '0';
              prefix[1] = uint8_t(hexadd + 81);
              prefixlen = 2;
            }
            break;
          case 'o':
            radix = 8;
            if (flags & kFlAlternate) flags |= kFlForceOctal;
            break;
          case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A':
            return kFloat;
          default:  // %n, %Z, and the letters the game's code has no case for
            return kOtherConv;
        }
        if (radix) {
          uint64_t number;
          const uint8_t* s = Slot(base, &ap, &args_first, &args_end);
          if (!s) return kMemory;
          if (flags & (kFlI64 | kFlLongLong)) {
            number = LdD(s);
          } else {
            const uint32_t v = LdW(s + 4);
            if (flags & kFlShort)
              number = (flags & kFlSigned) ? uint64_t(int64_t(int16_t(v))) : uint64_t(uint16_t(v));
            else
              number = (flags & kFlSigned) ? uint64_t(int64_t(int32_t(v))) : uint64_t(v);
          }
          if ((flags & kFlSigned) && int64_t(number) < 0) {
            number = 0 - number;
            flags |= kFlNegative;
          }
          if (!(flags & (kFlI64 | kFlLongLong))) number &= 0xFFFFFFFFull;
          if (int32_t(precision) < 0) {
            precision = 1;
          } else {
            flags &= ~kFlLeadZero;
            if (int32_t(precision) > 512) precision = 512;
          }
          if (number == 0) prefixlen = 0;
          uint8_t* p = numbuf + kNumEnd;
          for (;;) {
            const bool more = int32_t(precision) > 0;
            precision -= 1;
            if (!more && number == 0) break;
            const uint64_t q = number / radix;
            uint32_t d = uint32_t(number - q * radix) + 48u;
            number = q;
            if (int32_t(d) > 57) d += hexadd;
            *p-- = uint8_t(d);
          }
          textlen = int32_t((numbuf + kNumEnd) - p);
          text = p + 1;
          if ((flags & kFlForceOctal) && (textlen == 0 || text[0] != '0')) {
            *p = '0';
            text = p;
            ++textlen;
          }
          output = true;
        }
        break;
      }
      default:
        break;
    }
    if (output && !no_output) {
      if (flags & kFlSigned) {
        if (flags & kFlNegative) {
          prefix[0] = '-';
          prefixlen = 1;
        } else if (flags & kFlSign) {
          prefix[0] = '+';
          prefixlen = 1;
        } else if (flags & kFlSpace) {
          prefix[0] = ' ';
          prefixlen = 1;
        }
      }
      const int32_t padding = int32_t(width - uint32_t(textlen) - prefixlen);
      if (!(flags & (kFlLeft | kFlLeadZero)) && !w.Pad(' ', padding)) return kTooLong;
      if (!w.Str(prefix, int32_t(prefixlen))) return kTooLong;
      if ((flags & kFlLeadZero) && !(flags & kFlLeft) && !w.Pad('0', padding)) return kTooLong;
      if (!w.Str(text, textlen)) return kTooLong;
      if ((flags & kFlLeft) && !w.Pad(' ', padding)) return kTooLong;
    }
    ch = int8_t(f[i]);
  }

  o->n = w.n;
  if (args_end && !AddRead(o, args_first, uint64_t(args_end) - args_first)) return kTooLong;
  // Nothing it reads may sit in the stack the game's code would use for its
  // own frame first (below the caller's r1).
  const uint64_t frame_begin = r1 >= kGuestFrame ? uint64_t(r1) - kGuestFrame : 0;
  const uint64_t frame_size = uint64_t(r1) - frame_begin;
  for (int k = 0; k < o->reads; ++k)
    if (Overlap(o->read_begin[k], o->read_size[k], frame_begin, frame_size)) return kMemory;
  if (o->count_only || o->n == 0) return kNative;
  // write_char: the k-th character fits while _cnt - k is still >= 0 (32-bit).
  const int32_t cnt = int32_t(o->cnt);
  if (!(cnt == INT32_MIN || (cnt >= 0 && o->n <= uint32_t(cnt)))) return kFull;
  if (!Plain(base, o->ptr, o->n)) return kMemory;
  if (Overlap(o->ptr, o->n, frame_begin, frame_size)) return kMemory;
  for (int k = 0; k < o->reads; ++k)
    if (Overlap(o->ptr, o->n, o->read_begin[k], o->read_size[k])) return kMemory;
  return kNative;
}

// Writes a native result: the output at _ptr, _ptr and _cnt moved on (as the
// game's write_char leaves them), and the returned count.
void Commit(uint8_t* base, const Out& o, PPCContext& ctx) {
  if (!o.count_only && o.n) {
    std::memcpy(Host(base, o.ptr), o.buf, o.n);
    uint8_t* fs = Host(base, o.stream);
    StW(fs + kFilePtr, o.ptr + o.n);
    StW(fs + kFileCnt, o.cnt - o.n);
  }
  ctx.r3.u64 = o.n;  // the game's code returns it zero-extended (lwz)
}

// After the game's code ran a call the native version made too: the same
// value, stream and bytes?
bool SameAsGame(uint8_t* base, const Out& o, uint64_t game_r3, std::string* why) {
  char msg[160];
  if (game_r3 != uint64_t(o.n)) {
    std::snprintf(msg, sizeof(msg), "returned %lld, native %u",
                  static_cast<long long>(int64_t(game_r3)), o.n);
    *why = msg;
    return false;
  }
  const uint8_t* fs = Host(base, o.stream);
  const uint32_t ptr = LdW(fs + kFilePtr), cnt = LdW(fs + kFileCnt);
  const uint32_t want_ptr = o.count_only ? o.ptr : o.ptr + o.n;
  const uint32_t want_cnt = o.count_only ? o.cnt : o.cnt - o.n;
  if (ptr != want_ptr || cnt != want_cnt) {
    std::snprintf(msg, sizeof(msg), "the stream's _ptr/_cnt %08X/%08X, native %08X/%08X", ptr, cnt,
                  want_ptr, want_cnt);
    *why = msg;
    return false;
  }
  if (!o.count_only && o.n) {
    const uint8_t* g = Host(base, o.ptr);
    if (std::memcmp(g, o.buf, o.n) != 0) {
      uint32_t at = 0;
      while (at < o.n && g[at] == o.buf[at]) ++at;
      std::snprintf(msg, sizeof(msg), "character %u is 0x%02X, native 0x%02X", at, g[at],
                    o.buf[at]);
      *why = msg;
      return false;
    }
  }
  return true;
}

// The format for a log line: printable characters only, at most 80.
std::string Shown(uint8_t* base, uint32_t fmt) {
  std::string s;
  const uint64_t run = PlainRun(base, fmt);
  const uint8_t* p = Host(base, fmt);
  for (uint64_t k = 0; k < run && k < 80 && p[k]; ++k) {
    const uint8_t c = p[k];
    if (c >= 32 && c < 127) {
      s += char(c);
    } else {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "\\x%02X", c);
      s += hex;
    }
  }
  return s;
}

// ---------------------------------------------------------------------------
// Counters (per thread) and the report
// ---------------------------------------------------------------------------

struct Counters {
  std::atomic<uint64_t> native{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> checked{0};
  std::atomic<uint64_t> guest[kWhyCount]{};
  uint32_t tick = 0;  // this thread only
};

std::mutex g_counters_mutex;
std::vector<Counters*> g_counters;  // one per thread that ever called; never freed

Counters& Mine() {
  thread_local Counters* t = nullptr;
  if (!t) {
    t = new Counters();
    std::lock_guard lock(g_counters_mutex);
    g_counters.push_back(t);
  }
  return *t;
}

// Only the owning thread writes its counters.
inline void Bump(std::atomic<uint64_t>& c, uint64_t by = 1) {
  c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kFirstReportNs = int64_t(60) * 1000000000;   // a minute after the check
constexpr int64_t kReportEveryNs = int64_t(600) * 1000000000;  // then every 10 minutes
std::atomic<int64_t> g_next_report_ns{0};                      // 0: none planned
std::atomic<bool> g_differed{false};

void Report() {
  uint64_t native = 0, bytes = 0, checked = 0, guest[kWhyCount] = {};
  {
    std::lock_guard lock(g_counters_mutex);
    for (const Counters* c : g_counters) {
      native += c->native.load(std::memory_order_relaxed);
      bytes += c->bytes.load(std::memory_order_relaxed);
      checked += c->checked.load(std::memory_order_relaxed);
      for (int k = 0; k < kWhyCount; ++k) guest[k] += c->guest[k].load(std::memory_order_relaxed);
    }
  }
  std::string left;
  uint64_t total = 0;
  for (int k = 1; k < kWhyCount; ++k) {
    if (!guest[k]) continue;
    char part[96];
    std::snprintf(part, sizeof(part), "%s%s %llu", total ? ", " : "", kWhyName[k],
                  static_cast<unsigned long long>(guest[k]));
    left += part;
    total += guest[k];
  }
  REXLOG_INFO("cod3: native printf so far: {} calls native ({:.1f} KB written), {} compared with "
              "the game's code ({}); {} by the game's code{}{}{}",
              native, double(bytes) / 1024.0, checked,
              g_differed.load(std::memory_order_relaxed) ? "one DIFFERED" : "all the same", total,
              total ? " (" : "", left, total ? ")" : "");
}

void MaybeReport() {
  int64_t next = g_next_report_ns.load(std::memory_order_relaxed);
  if (next == 0) return;
  const int64_t now = NowNs();
  if (now < next) return;
  if (!g_next_report_ns.compare_exchange_strong(next, now + kReportEveryNs,
                                                std::memory_order_relaxed))
    return;  // another thread reports
  Report();
}

void ReportCommand(std::string_view) { Report(); }

// ---------------------------------------------------------------------------
// The check: random formats, the game's code against the native version
// ---------------------------------------------------------------------------

struct Rng {
  uint64_t s;
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint32_t Below(uint32_t n) { return n ? uint32_t(Next() % n) : 0; }  // 0 .. n-1
};

// Each scratch block (the same layout in both).
constexpr uint32_t kBlock = 0x10000;
constexpr uint32_t kOffFile = 0x0000;  // the FILE (32 bytes)
constexpr uint32_t kOffArgs = 0x0100;  // the argument slots
constexpr int kSlots = 64;
constexpr uint32_t kOffFmt = 0x0400;   // the format
constexpr uint32_t kFmtMax = 0x3F0;
constexpr uint32_t kOffPool = 0x0800;  // strings for %s, NUL-terminated, zeros at the end
constexpr uint32_t kPoolSize = 0x1000;
constexpr uint32_t kOffOut = 0x2000;  // the output
constexpr uint32_t kOutMax = 0xD000;  // up to 0xF000; the rest stays zeros
constexpr uint8_t kOutFill = 0xA5;

struct Block {
  uint32_t alloc = 0;
  uint32_t guest = 0;  // 4 KB aligned
  uint8_t* host = nullptr;
};

// One random case, independent of where the block sits.
struct Case {
  std::string fmt;
  uint64_t slot[kSlots] = {};
  bool slot_is_string[kSlots] = {};  // low word = kOffPool + offset in the block
  uint32_t string_off[kSlots] = {};
  uint32_t out_off = kOffOut;  // _ptr (block offset; 0: none, for count-only streams)
  uint32_t cnt = 0x7FFFFFFF;
  bool count_only = false;
  uint32_t flag = 0x42;  // _IOWRT | _IOSTRG, as sprintf makes it
  bool overlap = false;  // the output sits in the strings
};

struct Pool {
  std::vector<uint32_t> starts;  // string starts (pool offsets)
  uint8_t bytes[kPoolSize] = {};
};

// Characters the generator uses for literal text and strings: printable ASCII
// and high bytes that aren't lead bytes in the game's locale.
std::vector<uint8_t> LiteralChars(uint8_t* base, bool with_percent) {
  std::vector<uint8_t> out;
  const uint8_t* ctab = nullptr;
  if (Plain(base, kLocalePtr, 4)) {
    const uint32_t loc = LdW(Host(base, kLocalePtr));
    if (loc && Plain(base, uint64_t(loc) + 200, 4)) {
      const uint32_t t = LdW(Host(base, loc + 200));
      if (t && Plain(base, t, 512)) ctab = Host(base, t);
    }
  }
  for (int c = 1; c < 256; ++c) {
    if (c == '%' && !with_percent) continue;
    if (c < 32 && c != '\t' && c != '\n') continue;
    if (ctab && (LdH(ctab + 2 * c) & 0x8000)) continue;
    if (!ctab && c >= 128) continue;
    out.push_back(uint8_t(c));
  }
  return out;
}

void MakePool(Rng& rng, const std::vector<uint8_t>& chars, Pool* pool) {
  std::memset(pool->bytes, 0, sizeof(pool->bytes));
  pool->starts.clear();
  uint32_t at = 0;
  while (at + 80 < kPoolSize - 64) {
    pool->starts.push_back(at);
    const uint32_t len = rng.Below(4) == 0 ? 0 : rng.Below(rng.Below(3) ? 16 : 70);
    for (uint32_t k = 0; k < len; ++k) pool->bytes[at + k] = chars[rng.Below(uint32_t(chars.size()))];
    at += len + 1;
  }
}

uint64_t IntValue(Rng& rng) {
  switch (rng.Below(14)) {
    case 0: return 0;
    case 1: return rng.Below(10);
    case 2: return uint64_t(-int64_t(rng.Below(10)));
    case 3: return 0x7FFFFFFFull;
    case 4: return 0x80000000ull;
    case 5: return 0xFFFFFFFFull;
    case 6: return 0x8000000000000000ull;
    case 7: return ~0ull;
    case 8: return 0x100000000ull;
    case 9: return rng.Next() & 0xFFFF;
    case 10: return uint64_t(int64_t(int32_t(uint32_t(rng.Next()))));
    case 11: return 0x7FFFFFFFFFFFFFFFull;
    case 12: return (rng.Next() & 0xFFFFFFFF00000000ull) | 0x8000;
    default: return rng.Next();
  }
}

// A double for the floating point conversions (they go to the game's code).
uint64_t DoubleBits(Rng& rng) {
  static const double kSome[] = {0.0, -0.0, 1.0, -1.5, 3.14159, 1e10, 1e-10, 123456.789, 1e300, 5e-324};
  if (rng.Below(3) == 0) return rng.Next();
  double d = kSome[rng.Below(10)];
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}

// A well-formed format and its arguments. unsupported: mix in conversions that
// go to the game's code. strings_only: %s and literals only (overlap cases).
void MakeTyped(Rng& rng, const Pool& pool, const std::vector<uint8_t>& chars, bool unsupported,
               bool strings_only, Case* c) {
  int slot = 0;
  auto add_int = [&](uint64_t v) {
    if (slot < kSlots) c->slot[slot++] = v;
  };
  auto add_string = [&]() {
    if (slot >= kSlots) return;
    if (!strings_only && rng.Below(10) == 0) {
      c->slot[slot++] = 0;  // null: "(null)"
      return;
    }
    c->slot_is_string[slot] = true;
    c->string_off[slot] = pool.starts[rng.Below(uint32_t(pool.starts.size()))];
    ++slot;
  };
  const int segments = 1 + int(rng.Below(8));
  for (int sgm = 0; sgm < segments && slot + 3 <= kSlots && c->fmt.size() < kFmtMax - 40; ++sgm) {
    const uint32_t r = rng.Below(10);
    if (r < 3) {
      const uint32_t len = rng.Below(12);
      for (uint32_t k = 0; k < len; ++k) c->fmt += char(chars[rng.Below(uint32_t(chars.size()))]);
      continue;
    }
    if (r == 3) {
      c->fmt += "%%";
      continue;
    }
    std::string spec = "%";
    const uint32_t nflags = rng.Below(3) ? rng.Below(4) : 0;
    for (uint32_t k = 0; k < nflags; ++k) spec += "-+ #0"[rng.Below(5)];
    const uint32_t wr = rng.Below(10);
    if (wr >= 5 && wr < 8) {
      spec += std::to_string(1 + rng.Below(rng.Below(2) ? 9 : 120));
    } else if (wr >= 8) {
      spec += '*';
      add_int(uint64_t(int64_t(int32_t(rng.Below(170)) - 40)) ^ (rng.Next() & 0xFFFFFFFF00000000ull));
    }
    const uint32_t pr = rng.Below(10);
    if (pr == 5) {
      spec += '.';
    } else if (pr >= 6 && pr < 8) {
      spec += '.';
      spec += std::to_string(rng.Below(rng.Below(2) ? 10 : 140));
    } else if (pr >= 8) {
      spec += ".*";
      add_int(uint64_t(int64_t(int32_t(rng.Below(140)) - 8)) ^ (rng.Next() & 0xFFFFFFFF00000000ull));
    }
    if (strings_only) {
      c->fmt += spec + 's';
      add_string();
      continue;
    }
    static const char* kSizes[] = {"", "", "", "", "h", "l", "ll", "I64", "I32"};
    const char* size = kSizes[rng.Below(9)];
    const uint32_t tr = rng.Below(unsupported ? 16 : 12);
    char type;
    if (tr < 9) {
      type = "diouxXcsp"[tr];
    } else if (tr < 12) {
      type = tr == 9 ? 's' : tr == 10 ? 'd' : 'x';
      if (tr == 9 && rng.Below(2)) {
        size = "h";
        type = rng.Below(2) ? 'S' : 'C';  // narrow with h
      }
    } else {
      type = "eEfgGaA"[rng.Below(7)];
      size = "";
    }
    if (type == 'c' || type == 'C' || type == 's' || type == 'S') {
      if (std::strcmp(size, "h") != 0) size = "";
    }
    if (type == 'p') size = "";
    c->fmt += spec + size + type;
    switch (type) {
      case 's':
      case 'S': add_string(); break;
      case 'c':
      case 'C': add_int(rng.Next()); break;
      case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A':
        add_int(DoubleBits(rng));
        break;
      default: add_int(IntValue(rng)); break;
    }
  }
  if (unsupported && rng.Below(3) == 0 && slot < kSlots) {
    // a wide conversion with a null or small argument (the game's code takes the call)
    static const char* kWideConv[] = {"%S", "%ls", "%ws", "%C", "%lc", "%wc"};
    const uint32_t k = rng.Below(6);
    c->fmt += kWideConv[k];
    add_int(k < 3 ? 0 : uint64_t(32 + rng.Below(90)));
  }
}

// Random format soup (no 'n', no lead bytes): every argument is 0, which is
// safe however the slots are read (a null string prints "(null)"). full: with
// the letters of the conversions that go to the game's code (floating point,
// wide, %Z); the start-up check leaves those to the tests.
void MakeSoup(Rng& rng, const std::vector<uint8_t>& high, bool full, Case* c) {
  static const char kSoupFull[] =
      "%%%%%%%%-+ #0123456789..**hlIwL6432diouxXcspCSZegfEGaAqyzbBHjkmrtv!@,;:'\"\\/";
  static const char kSoupStart[] =
      "%%%%%%%%-+ #0123456789..**hIL6432diouxXcspqyzbBHjkmrtv!@,;:'\"\\/";
  const char* soup = full ? kSoupFull : kSoupStart;
  const uint32_t soup_len = uint32_t(std::strlen(soup));
  const uint32_t len = 1 + rng.Below(40);
  int digits = 0;  // at most two digits in a row: no float precision above 99
  for (uint32_t k = 0; k < len; ++k) {
    char ch;
    if (!high.empty() && rng.Below(16) == 0) {
      ch = char(high[rng.Below(uint32_t(high.size()))]);
    } else {
      ch = soup[rng.Below(soup_len)];
    }
    if (ch >= '0' && ch <= '9') {
      if (digits == 2) ch = '.';
      ++digits;
    } else {
      digits = 0;
    }
    c->fmt += ch;
  }
}

// full: also the conversions that go to the game's code (the tests; the
// start-up check leaves the game's floating point and wide character code
// alone).
void MakeCase(Rng& rng, const Pool& pool, const std::vector<uint8_t>& chars,
              const std::vector<uint8_t>& high, bool full, Case* c) {
  *c = Case();
  const uint32_t kind = rng.Below(100);
  if (kind < 55 || (kind < 70 && !full)) {
    MakeTyped(rng, pool, chars, false, false, c);
  } else if (kind < 70) {
    MakeTyped(rng, pool, chars, true, false, c);
  } else if (kind < 85) {
    MakeSoup(rng, high, full, c);
  } else if (kind < 97) {
    // stream edges: small counts, no buffer, other flags
    MakeTyped(rng, pool, chars, false, false, c);
    const uint32_t e = rng.Below(6);
    if (e == 0) {
      c->count_only = true;
      c->out_off = 0;
      c->cnt = rng.Below(3) ? 0 : uint32_t(rng.Below(100));
    } else if (e == 1) {
      c->cnt = 0x80000000u;
    } else if (e == 2) {
      c->cnt = uint32_t(int32_t(rng.Below(5)) - 3);
    } else {
      c->cnt = rng.Below(90);
    }
    if (rng.Below(4) == 0) c->flag = rng.Below(2) ? 0x40 : 0x4A;
  } else {
    // output over the strings it prints
    MakeTyped(rng, pool, chars, false, true, c);
    c->overlap = true;
    c->out_off = kOffPool + pool.starts[rng.Below(uint32_t(pool.starts.size() / 2))];
  }
  if (c->fmt.size() > kFmtMax) c->fmt.resize(kFmtMax);
}

// The argument slots the game's code reads for a format, in order: 'p' a
// string pointer (%s %S), 'z' a counted string's (%Z), 'n' %n's, 'v' a value.
// Its state machine with the game's table, as Format runs it. False: a state
// past the table.
bool PlanSlots(const uint8_t* f, const uint8_t* lk, std::vector<char>* kinds) {
  kinds->clear();
  uint32_t state = 0, i = 0;
  int ch = int8_t(f[0]);
  while (ch != 0) {
    ++i;
    const uint32_t cls = (ch >= 32 && ch <= 120) ? (lk[ch - 32] & 0xFu) : 0u;
    state = uint32_t(int32_t(int8_t(lk[cls * 8 + state])) >> 4);
    if (state > 7) return false;
    if ((state == 3 || state == 5) && ch == '*') kinds->push_back('v');
    if (state == 6) {
      if (ch == 'I') {
        const int c0 = int8_t(f[i]);
        if ((c0 == '6' && f[i + 1] == '4') || (c0 == '3' && f[i + 1] == '2')) {
          i += 2;
        } else if (!(c0 == 'd' || c0 == 'i' || c0 == 'o' || c0 == 'u' || c0 == 'x' ||
                     c0 == 'X')) {
          state = 0;
        }
      } else if (ch == 'l' && f[i] == 'l') {
        ++i;
      }
    }
    if (state == 7) {
      switch (ch) {
        case 's': case 'S': kinds->push_back('p'); break;
        case 'Z': kinds->push_back('z'); break;
        case 'n': kinds->push_back('n'); break;
        case 'c': case 'C': case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p':
        case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A':
          kinds->push_back('v');
          break;
        default:
          break;
      }
    }
    ch = int8_t(f[i]);
  }
  return true;
}

// Whether the game's code can run a case safely: every slot it reads as a
// string pointer holds one of the case's strings or 0, %Z only 0, no %n (its
// count output is off, so the game's code would report an invalid
// parameter), and no slot past the case's.
bool SafeForGame(const Case& c, const uint8_t* lk, std::vector<char>* kinds) {
  if (!PlanSlots(reinterpret_cast<const uint8_t*>(c.fmt.c_str()), lk, kinds)) return false;
  if (kinds->size() > size_t(kSlots)) return false;
  for (size_t k = 0; k < kinds->size(); ++k) {
    const char kind = (*kinds)[k];
    const bool zero = !c.slot_is_string[k] && uint32_t(c.slot[k]) == 0;
    if (kind == 'n') return false;
    if (kind == 'z' && !zero) return false;
    if (kind == 'p' && !c.slot_is_string[k] && !zero) return false;
  }
  return true;
}

// Writes a case into a block (its addresses).
void Place(const Case& c, const Pool& pool, const Block& b) {
  uint8_t* h = b.host;
  std::memset(h + kOffFile, 0, 0x100);
  const uint32_t out = c.out_off ? b.guest + c.out_off : 0;
  StW(h + kOffFile + kFilePtr, out);
  StW(h + kOffFile + kFileCnt, c.cnt);
  StW(h + kOffFile + kFileBase, c.count_only ? 0 : out);
  StW(h + kOffFile + kFileFlag, c.flag);
  for (int k = 0; k < kSlots; ++k) {
    const uint64_t v = c.slot_is_string[k] ? uint64_t(b.guest + kOffPool + c.string_off[k]) : c.slot[k];
    StD(h + kOffArgs + 8 * k, v);
  }
  std::memset(h + kOffFmt, 0, 0x400);
  std::memcpy(h + kOffFmt, c.fmt.data(), c.fmt.size());
  std::memcpy(h + kOffPool, pool.bytes, kPoolSize);
}

// The game's code on one block (r1 below the caller's red zone, as guest::Call
// does); returns r3.
uint64_t RunGame(PPCContext& parent, uint8_t* base, const Block& b, uint64_t junk) {
  rex::CallFrame frame(parent);
  PPCContext& c = frame.ctx;
  c.r1.u32 = (parent.r1.u32 - 0x200) & ~0xFu;
  c.r3.u64 = b.guest + kOffFile;
  c.r4.u64 = b.guest + kOffFmt;
  c.r5.u64 = junk;  // the locale argument, unused by this code
  c.r6.u64 = (b.guest + kOffArgs) | (junk & 0xFFFFFFFF00000000ull);
  __imp__sub_82353B58(c, base);
  return c.r3.u64;
}

// The hook's path on one block: native where Format allows, else the game's
// code. Returns r3; *native: whether it ran natively.
uint64_t RunHookPath(PPCContext& parent, uint8_t* base, const Block& b, uint64_t junk,
                     bool* native) {
  rex::CallFrame frame(parent);
  PPCContext& c = frame.ctx;
  c.r1.u32 = (parent.r1.u32 - 0x200) & ~0xFu;
  c.r3.u64 = b.guest + kOffFile;
  c.r4.u64 = b.guest + kOffFmt;
  c.r5.u64 = junk;
  c.r6.u64 = (b.guest + kOffArgs) | (junk & 0xFFFFFFFF00000000ull);
  Out o;
  *native = Format(base, c.r1.u32, c.r3.u32, c.r4.u32, c.r6.u32, &o) == kNative;
  if (*native)
    Commit(base, o, c);
  else
    __imp__sub_82353B58(c, base);
  return c.r3.u64;
}

// The block's pointers as offsets, so the two blocks compare byte for byte.
void Normalize(const Case& c, const Block& b) {
  uint8_t* h = b.host;
  for (uint32_t field : {kFilePtr, kFileBase}) {
    const uint32_t v = LdW(h + kOffFile + field);
    if (v >= b.guest && v < b.guest + kBlock) StW(h + kOffFile + field, v - b.guest);
  }
  for (int k = 0; k < kSlots; ++k)
    if (c.slot_is_string[k]) StD(h + kOffArgs + 8 * k, c.string_off[k]);
}

struct CheckResult {
  int cases = 0;
  int native = 0;
  int skipped = 0;  // cases the game's code couldn't run safely (never run)
  bool same = true;
  std::string first_diff;
  double game_ns = 0, native_ns = 0;
};

// count random cases; both blocks are kBlock bytes of scratch guest memory.
CheckResult RunCases(PPCContext& ctx, uint8_t* base, const Block& a, const Block& b, int count,
                     uint64_t seed, bool full) {
  CheckResult res;
  Rng rng{seed};
  const uint8_t* lk = Host(base, kLookup);
  std::vector<char> kinds;
  const std::vector<uint8_t> chars = LiteralChars(base, false);
  std::vector<uint8_t> high;
  for (uint8_t ch : chars)
    if (ch >= 128) high.push_back(ch);
  Pool pool;
  MakePool(rng, chars, &pool);
  std::memset(a.host, 0, kBlock);
  std::memset(a.host + kOffOut, kOutFill, kOutMax);
  std::memcpy(b.host, a.host, kBlock);
  int64_t game_ns = 0, native_ns = 0;
  int timed_native = 0;
  Case c;
  for (int i = 0; i < count; ++i) {
    if (i % 512 == 511) MakePool(rng, chars, &pool);
    MakeCase(rng, pool, chars, high, full, &c);
    if (!SafeForGame(c, lk, &kinds)) {
      ++res.skipped;
      continue;
    }
    Place(c, pool, a);
    Place(c, pool, b);
    const uint64_t junk = rng.Below(8) == 0 ? rng.Next() : 0;
    const int64_t t0 = NowNs();
    const uint64_t r_game = RunGame(ctx, base, a, junk);
    const int64_t t1 = NowNs();
    bool native = false;
    const uint64_t r_native = RunHookPath(ctx, base, b, junk, &native);
    const int64_t t2 = NowNs();
    ++res.cases;
    if (native) {
      ++res.native;
      game_ns += t1 - t0;
      native_ns += t2 - t1;
      ++timed_native;
    }
    Normalize(c, a);
    Normalize(c, b);
    const uint64_t most = std::max(uint32_t(r_game), uint32_t(r_native));
    const uint32_t extent = uint32_t(std::min<uint64_t>(kOutMax, most + 256));
    bool same = r_game == r_native && std::memcmp(a.host, b.host, kOffOut) == 0 &&
                std::memcmp(a.host + kOffOut, b.host + kOffOut, extent) == 0;
    if (!same && res.same) {
      res.same = false;
      uint32_t at = 0;
      while (at < kOffOut + extent && a.host[at] == b.host[at]) ++at;
      std::string shown;
      for (char ch : c.fmt) {
        if (ch >= 32 && ch < 127) {
          shown += ch;
        } else {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\x%02X", uint8_t(ch));
          shown += hex;
        }
      }
      char msg[320];
      std::snprintf(msg, sizeof(msg),
                    "format \"%.120s\" (%s): the game's code returned %lld, native %lld; block "
                    "byte +0x%X: 0x%02X, native 0x%02X",
                    shown.c_str(), native ? "native" : "the game's code both times",
                    static_cast<long long>(int64_t(r_game)), static_cast<long long>(int64_t(r_native)),
                    at, at < kBlock ? a.host[at] : 0, at < kBlock ? b.host[at] : 0);
      res.first_diff = msg;
    }
    // Back to the same state for the next case.
    std::memset(a.host + kOffOut, kOutFill, extent);
    std::memset(b.host + kOffOut, kOutFill, extent);
  }
  if (timed_native) {
    res.game_ns = double(game_ns) / timed_native;
    res.native_ns = double(native_ns) / timed_native;
  }
  return res;
}

bool AllocBlock(rex::memory::Memory* mem, uint8_t* base, Block* b) {
  b->alloc = mem->SystemHeapAlloc(kBlock + 0x1000);
  if (!b->alloc) return false;
  b->guest = (b->alloc + 0xFFFu) & ~0xFFFu;
  if (!Plain(base, b->guest, kBlock)) return false;
  b->host = Host(base, b->guest);
  return true;
}

// 0: not checked yet, 1: a thread is checking now, 2: checked.
std::atomic<int> g_check_state{0};
std::atomic<bool> g_ok{false};
std::atomic<int> g_live_checks{0};  // the game's first native calls, run both ways
constexpr int kLiveChecks = 64;
constexpr uint32_t kSpotEvery = 1024;  // a power of two
std::atomic<int64_t> g_test_pending{0};

// Runs count random cases and logs them. initial: the start-up check (sets
// whether the native version runs). False: no scratch memory yet.
bool RunCheck(PPCContext& ctx, uint8_t* base, int count, bool initial) {
  auto* ks = rex::system::kernel_state();
  auto* mem = ks ? ks->memory() : nullptr;
  if (!mem) return false;
  Block a, b;
  if (!AllocBlock(mem, base, &a) || !AllocBlock(mem, base, &b)) {
    if (a.alloc) mem->SystemHeapFree(a.alloc);
    if (b.alloc) mem->SystemHeapFree(b.alloc);
    REXLOG_WARN("cod3: native printf: no scratch memory for its check");
    return false;
  }
  const int64_t start = NowNs();
  const CheckResult r =
      RunCases(ctx, base, a, b, count, initial ? 0x2545F4914F6CDD1Dull : uint64_t(NowNs()) | 1,
               !initial);
  mem->SystemHeapFree(a.alloc);
  mem->SystemHeapFree(b.alloc);
  const double ms = double(NowNs() - start) / 1e6;
  if (!r.same) {
    g_ok.store(false, std::memory_order_relaxed);
    g_differed.store(true, std::memory_order_relaxed);
    REXLOG_ERROR("cod3: native printf differs from the game's own code ({}); it stays on the "
                 "game's code",
                 r.first_diff);
  } else if (initial) {
    g_ok.store(r.native > 0, std::memory_order_relaxed);
  }
  REXLOG_INFO("cod3: native printf {} ({:.1f} ms): {} random formats ({} left out as unsafe for the "
              "game's code), {} run natively{}; the game's code {:.2f} us a call, native {:.2f} "
              "us{}",
              initial ? "self-check" : "test", ms, r.cases, r.skipped, r.native,
              r.same ? " and all the same" : ", one DIFFERED", r.game_ns / 1000.0,
              r.native_ns / 1000.0,
              initial ? (g_ok.load(std::memory_order_relaxed) ? "; now native (cod3_native_printf)"
                                                              : "; it stays off")
                      : "");
  if (initial) {
    g_live_checks.store(kLiveChecks, std::memory_order_relaxed);
    g_next_report_ns.store(NowNs() + kFirstReportNs, std::memory_order_relaxed);
  }
  return true;
}

void TestCommand(std::string_view args) {
  int64_t n = 100000;
  if (!args.empty()) {
    int64_t v = 0;
    bool digits = false;
    for (char ch : args) {
      if (ch >= '0' && ch <= '9') {
        v = std::min<int64_t>(v * 10 + (ch - '0'), 100000000);
        digits = true;
      } else if (ch != ' ' && ch != '\t') {
        digits = false;
        break;
      }
    }
    if (digits) n = v;
  }
  n = std::clamp<int64_t>(n, 1000, 1000000);
  g_test_pending.store(n, std::memory_order_relaxed);
  REXLOG_INFO("cod3: native printf test: {} random formats on the next sprintf-style call (the "
              "game pauses while it runs)",
              n);
}

// Called by the hook until the check has run. True once it ran (on this or
// another thread); while another thread runs it, false (that call takes the
// game's code).
bool EnsureChecked(PPCContext& ctx, uint8_t* base) {
  int expected = 0;
  if (!g_check_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    return expected == 2;
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory() || ks->memory()->virtual_membase() != base ||
      !Plain(base, kLookup, kLookupSize) || !Plain(base, kNullStringPtr, 4) ||
      !Plain(base, kLocalePtr, 4) || !Plain(base, kIob, 96)) {
    REXLOG_WARN("cod3: native printf off: guest memory isn't where the game's code expects it");
    g_check_state.store(2, std::memory_order_release);  // stays off
    return true;
  }
  const bool ran = RunCheck(ctx, base, 2500, true);
  g_check_state.store(ran ? 2 : 0, std::memory_order_release);
  return ran;
}

inline bool Use(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cod3_native_printf)) return false;
  if (g_check_state.load(std::memory_order_acquire) != 2 && !EnsureChecked(ctx, base))
    return false;
  if (g_test_pending.load(std::memory_order_relaxed) != 0) {
    const int64_t n = g_test_pending.exchange(0, std::memory_order_relaxed);
    if (n > 0) RunCheck(ctx, base, int(n), false);
  }
  return g_ok.load(std::memory_order_relaxed);
}

// Whether this native call runs both ways: the game's first ones, then one in
// kSpotEvery per thread (every one with cod3_native_printf_check_all).
inline bool Checking(Counters& c) {
  if (g_live_checks.load(std::memory_order_relaxed) > 0 &&
      g_live_checks.fetch_sub(1, std::memory_order_relaxed) > 0)
    return true;
  if (((++c.tick) & (kSpotEvery - 1)) == 0) {
    MaybeReport();
    return true;
  }
  return REXCVAR_GET(cod3_native_printf_check_all);
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_printf_report, ReportCommand, "cod3/Performance",
                            "cod3_native_printf_report: how many sprintf-style calls ran as "
                            "native code and why the rest didn't, written to the log");
REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_printf_test, TestCommand, "cod3/Performance",
                            "cod3_native_printf_test N: compares N more random formats (1000 to "
                            "1000000, default 100000) with the game's code on the next "
                            "sprintf-style call, written to the log");

// ---------------------------------------------------------------------------
// The hook: native where Use() and Format's rules allow, else the original
// code. A checked call runs the original (its result stays) and compares.
// ---------------------------------------------------------------------------

// _output_l (cod3mp sub_82323758).
REX_HOOK_RAW(sub_82353B58) {
  if (Use(ctx, base)) {
    Counters& c = Mine();
    Out o;
    const Why why = Format(base, ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, &o);
    if (why == kNative) {
      if (Checking(c)) {
        __imp__sub_82353B58(ctx, base);
        Bump(c.checked);
        std::string diff;
        if (!SameAsGame(base, o, ctx.r3.u64, &diff)) {
          g_ok.store(false, std::memory_order_relaxed);
          if (!g_differed.exchange(true)) {
            REXLOG_ERROR("cod3: native printf differed from the game's own code in a call "
                         "(format \"{}\": {}); the game's code runs from now on",
                         Shown(base, o.fmt), diff);
          }
        }
        return;
      }
      Commit(base, o, ctx);
      Bump(c.native);
      Bump(c.bytes, o.n);
      return;
    }
    Bump(c.guest[why]);
  }
  __imp__sub_82353B58(ctx, base);
}
