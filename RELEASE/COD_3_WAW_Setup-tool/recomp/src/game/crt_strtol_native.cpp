// cod3 - [native-strtol] the C runtime's string-to-integer conversion as
// native code: strtoxl, the function under atoi, strtol and strtoul.
//
// This is the Xbox 360 C runtime's own PowerPC function (189 instructions),
// translated by the recompiler; atoi alone has 74 call sites in the campaign
// (dvars, configs, script and menu strings). The native version walks the
// string the way the game's code does:
//   - spaces skipped by the locale's ctype table (the game's own table, read
//     from its memory; a multibyte locale goes to the game's code), then one
//     '+' or '-';
//   - base 0: "0x"/"0X" means 16, a leading '0' 8, else 10; base 16 skips a
//     "0x" too;
//   - digits by the ctype table's digit bit, letters by its letter bits
//     (lowercase made uppercase by the ASCII test the code makes), each below
//     the base;
//   - the value in 32 bits; the result register as the code leaves it (a
//     negative result is the 64-bit negation of the 32-bit value);
//   - *endptr: the first byte not used, or the string itself when there were
//     no digits.
// Everything that touches errno goes to the game's code, decided before
// anything is written: a value that doesn't fit (the code sets ERANGE), a
// null string or a base outside 0 and 2-36 (the invalid parameter handler).
// So do a string that runs past plain guest memory (see crt_native.cpp) and
// an endptr that overlaps what the call reads or the stack below the caller.
//
// Before the first native call, a check runs the game's code and the native
// version on thousands of random strings (spaces, signs, prefixes, digits and
// letters in every base, values at the limits, other bytes, with and without
// endptr, strtol and strtoul) and compares the result and *endptr; cases the
// native version leaves to the game's code (they would set errno) are not
// run. If one differs, the native version stays off for the session (logged).
// The game's first calls are compared too, then one in kSpotEvery per thread.
// cod3_native_strtol (on) switches it off at any time;
// cod3_native_strtol_check_all (off) compares every call (tests);
// cod3_native_strtol_test N runs N more random strings on the next call;
// cod3_native_strtol_report writes the counts to the log.
//
// Both programs: cod3mp has the same function (sub_82320008; only the locale
// data's address differs, COD3_ADDR) and tools/mp_aliases.py points the hook
// at it (mp/config/campaign_map.tsv).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

REXCVAR_DEFINE_BOOL(cod3_native_strtol, true, "cod3/Performance",
                    "The C runtime's string-to-integer conversion (atoi, strtol, strtoul) runs as "
                    "native code where that does exactly what the game's own code does (checked at "
                    "start; off: the translated Xbox 360 code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_native_strtol_check_all, false, "cod3/Performance",
                    "Testing: every native atoi/strtol call runs both ways and the results are "
                    "compared (slower)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82350BA8);  // strtoxl (cod3mp sub_82320008)

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
inline void StW(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

inline bool Overlap(uint64_t a, uint64_t na, uint64_t b, uint64_t nb) {
  return a < b + nb && b < a + na;
}

// The locale data's address (the code reads the global, not its r3); +172 is
// the multibyte length, +200 the ctype table.
constexpr uint32_t kLocalePtr = COD3_ADDR(0x825C7048, 0x825CC558);
constexpr uint32_t kGuestFrame = 0x400;  // the stack the game's code would use below r1

// strtoxl's flags (r24 there; the callers pass 0 for strtol, 1 for strtoul).
constexpr uint64_t kUnsigned = 1, kNeg = 2, kOverflow = 4, kReadDigit = 8;

enum Why : int {
  kNative = 0,
  kInvalid,     // a null string or a base outside 0 and 2-36
  kRange,       // the value doesn't fit: the game's code sets ERANGE
  kMultibyte,   // a multibyte locale (the code calls _isctype_l)
  kMemory,      // not plain guest memory, or endptr overlaps what the call reads
  kWhyCount
};
constexpr const char* kWhyName[kWhyCount] = {"native", "invalid arguments", "out of range",
                                             "multibyte locale", "memory"};

struct Result {
  uint64_t r3 = 0;
  uint32_t endptr = 0;  // where *endptr goes (0: none)
  uint32_t end = 0;     // its value
  uint32_t str = 0;
  uint32_t read = 0;  // bytes of the string read
};

// strtoxl(r4 = str, r5 = endptr, r6 = base, r7 = flags), as the game's code
// does it, into *o (nothing written). r1: the caller's stack pointer.
Why Parse(uint8_t* base, uint32_t r1, uint32_t str, uint32_t endptr, uint32_t ibase,
          uint64_t flags, Result* o) {
  o->str = str;
  o->endptr = endptr;
  o->read = 0;
  if (str == 0) return kInvalid;
  int32_t b = int32_t(ibase);
  if (b != 0 && (b < 2 || b > 36)) return kInvalid;
  if (!Plain(base, kLocalePtr, 4)) return kMemory;
  const uint32_t loc = LdW(Host(base, kLocalePtr));
  if (!Plain(base, uint64_t(loc) + 172, 32)) return kMemory;
  if (int32_t(LdW(Host(base, loc + 172))) > 1) return kMultibyte;
  const uint32_t table = LdW(Host(base, loc + 200));
  if (!Plain(base, table, 512)) return kMemory;
  const uint8_t* ct = Host(base, table);
  auto ctype = [&](uint32_t c) { return LdH(ct + ((c << 1) & 0x1FE)); };

  const uint64_t run = PlainRun(base, str);
  const uint8_t* s = Host(base, str);
  uint64_t i = 0;  // bytes read (the code's r29 is str + i)
  uint32_t c = 0;
  auto next = [&]() -> bool {
    if (i >= run) return false;
    c = s[i++];
    return true;
  };
  auto peek = [&](uint64_t k, uint32_t* v) -> bool {  // s[i + k], not consumed
    if (i + k >= run) return false;
    *v = s[i + k];
    return true;
  };

  if (!next()) return kMemory;
  while (ctype(c) & 8)
    if (!next()) return kMemory;
  if (int8_t(c) == '-') {
    flags |= kNeg;
    if (!next()) return kMemory;
  } else if (int8_t(c) == '+') {
    if (!next()) return kMemory;
  }
  bool skip_0x = false;
  if (b == 0) {
    if (int8_t(c) != '0') {
      b = 10;
    } else {
      uint32_t n;
      if (!peek(0, &n)) return kMemory;
      if (int8_t(n) == 'x' || int8_t(n) == 'X') {
        b = 16;
        skip_0x = true;
      } else {
        b = 8;
      }
    }
  } else if (b == 16) {
    skip_0x = true;
  }
  if (skip_0x && int8_t(c) == '0') {
    uint32_t n;
    if (!peek(0, &n)) return kMemory;
    if (int8_t(n) == 'x' || int8_t(n) == 'X') {
      uint32_t after;
      if (!peek(1, &after)) return kMemory;
      i += 2;
      c = after;
    }
  }

  const uint32_t ub = uint32_t(b);
  const uint32_t maxval = 0xFFFFFFFFu / ub;
  const uint32_t maxdigit = 0xFFFFFFFFu - maxval * ub;  // the code's r10 at the limit
  uint64_t number = 0;  // r26 there (64-bit)
  for (;;) {
    const uint16_t e = ctype(c);
    uint64_t d;
    if (e & 4) {
      d = uint64_t(int64_t(int8_t(c)) - 48);
    } else if (e & 0x103) {
      int64_t v = int8_t(c);
      if (v >= 97 && v <= 122) v -= 32;
      d = uint64_t(v - 55);
    } else {
      break;
    }
    if (uint32_t(d) >= ub) break;
    flags |= kReadDigit;
    const uint32_t n32 = uint32_t(number);
    if (n32 < maxval || (n32 == maxval && uint32_t(d) <= maxdigit)) {
      number = uint64_t(int64_t(int32_t(n32)) * int64_t(int32_t(ub))) + d;
    } else {
      return kRange;  // the code sets kOverflow, then ERANGE at the end
    }
    if (!next()) return kMemory;
  }
  // The code steps back to the byte that stopped it.
  uint32_t end = str + uint32_t(i - 1);
  if (!(flags & kReadDigit)) {
    end = str;
    number = 0;
  } else if (!(flags & kUnsigned)) {
    if ((flags & kNeg) ? uint32_t(number) > 0x80000000u : uint32_t(number) > 0x7FFFFFFFu)
      return kRange;
  }
  if (flags & kNeg) number = 0 - number;
  o->r3 = number;
  o->end = end;
  o->read = uint32_t(i);

  // Nothing it reads may sit in the stack the game's code would use first,
  // and endptr (written first with the string's address) may not overlap
  // anything read.
  const uint64_t frame_begin = r1 >= kGuestFrame ? uint64_t(r1) - kGuestFrame : 0;
  const uint64_t frame_size = uint64_t(r1) - frame_begin;
  if (Overlap(str, i, frame_begin, frame_size)) return kMemory;
  if (endptr) {
    if (!Plain(base, endptr, 4)) return kMemory;
    if (Overlap(endptr, 4, str, i) || Overlap(endptr, 4, frame_begin, frame_size) ||
        Overlap(endptr, 4, kLocalePtr, 4) || Overlap(endptr, 4, uint64_t(loc) + 172, 32) ||
        Overlap(endptr, 4, table, 512))
      return kMemory;
  }
  return kNative;
}

void Commit(uint8_t* base, const Result& o, PPCContext& ctx) {
  if (o.endptr) StW(Host(base, o.endptr), o.end);
  ctx.r3.u64 = o.r3;
}

// ---------------------------------------------------------------------------
// Counters and the report
// ---------------------------------------------------------------------------

struct Counters {
  std::atomic<uint64_t> native{0};
  std::atomic<uint64_t> checked{0};
  std::atomic<uint64_t> guest[kWhyCount]{};
  uint32_t tick = 0;
};

std::mutex g_counters_mutex;
std::vector<Counters*> g_counters;

Counters& Mine() {
  thread_local Counters* t = nullptr;
  if (!t) {
    t = new Counters();
    std::lock_guard lock(g_counters_mutex);
    g_counters.push_back(t);
  }
  return *t;
}

inline void Bump(std::atomic<uint64_t>& c, uint64_t by = 1) {
  c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kFirstReportNs = int64_t(60) * 1000000000;
constexpr int64_t kReportEveryNs = int64_t(600) * 1000000000;
std::atomic<int64_t> g_next_report_ns{0};
std::atomic<bool> g_differed{false};

void Report() {
  uint64_t native = 0, checked = 0, guest[kWhyCount] = {};
  {
    std::lock_guard lock(g_counters_mutex);
    for (const Counters* c : g_counters) {
      native += c->native.load(std::memory_order_relaxed);
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
  REXLOG_INFO("cod3: native strtol so far: {} calls native, {} compared with the game's code ({}); "
              "{} by the game's code{}{}{}",
              native, checked,
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
    return;
  Report();
}

void ReportCommand(std::string_view) { Report(); }

// ---------------------------------------------------------------------------
// The check: random strings, the game's code against the native version
// ---------------------------------------------------------------------------

struct Rng {
  uint64_t s;
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint32_t Below(uint32_t n) { return n ? uint32_t(Next() % n) : 0; }
};

constexpr uint32_t kBlock = 0x1000;
constexpr uint32_t kOffStr = 0x100;   // the string (up to 0x600 bytes, NUL-terminated)
constexpr uint32_t kOffEnd = 0x40;    // *endptr for the game's code
constexpr uint32_t kOffEnd2 = 0x80;   // *endptr for the native version
constexpr uint32_t kStrMax = 0x5F0;

void MakeString(Rng& rng, std::string* out, int32_t* b) {
  static const int32_t kBases[] = {0, 0, 10, 10, 10, 16, 16, 8, 2, 36};
  *b = rng.Below(5) == 0 ? int32_t(2 + rng.Below(35)) : kBases[rng.Below(10)];
  std::string& s = *out;
  s.clear();
  static const char kSpaces[] = " \t\n\v\f\r";
  const uint32_t spaces = rng.Below(3) ? 0 : rng.Below(5);
  for (uint32_t k = 0; k < spaces; ++k) s += kSpaces[rng.Below(6)];
  switch (rng.Below(5)) {
    case 0: s += '-'; break;
    case 1: s += '+'; break;
    default: break;
  }
  switch (rng.Below(6)) {
    case 0: s += "0x"; break;
    case 1: s += "0X"; break;
    case 2: s += '0'; break;
    default: break;
  }
  static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const uint32_t r = rng.Below(10);
  uint32_t ndig = r < 4 ? rng.Below(4) : r < 8 ? rng.Below(11) : rng.Below(40);
  if (rng.Below(6) == 0) {
    // values at the limits
    static const char* kEdges[] = {"2147483647", "2147483648", "4294967295", "4294967296",
                                   "7fffffff",   "80000000",   "ffffffff",   "100000000",
                                   "37777777777", "40000000000", "1z141z3",   "1z141z4"};
    s += kEdges[rng.Below(12)];
    ndig = 0;
  }
  for (uint32_t k = 0; k < ndig; ++k) s += kDigits[rng.Below(62)];
  // whatever follows
  const uint32_t tail = rng.Below(4);
  for (uint32_t k = 0; k < tail; ++k) {
    char ch = char(1 + rng.Below(255));
    s += ch;
  }
  if (s.size() > kStrMax) s.resize(kStrMax);
}

struct CheckResult {
  int cases = 0;
  int native = 0;
  bool same = true;
  std::string first_diff;
  double game_ns = 0, native_ns = 0;
};

CheckResult RunCases(PPCContext& ctx, uint8_t* base, uint32_t block, int count, uint64_t seed) {
  CheckResult res;
  Rng rng{seed};
  uint8_t* h = Host(base, block);
  std::string str;
  int64_t game_ns = 0, native_ns = 0;
  for (int i = 0; i < count; ++i) {
    int32_t b = 10;
    MakeString(rng, &str, &b);
    std::memset(h, 0, kBlock);
    std::memcpy(h + kOffStr, str.data(), str.size());
    const bool with_end = rng.Below(2) == 0;
    const uint64_t flags = rng.Below(4) == 0 ? 1 : 0;  // strtoul or strtol
    const uint64_t junk = rng.Below(8) == 0 ? (rng.Next() & 0xFFFFFFFF00000000ull) : 0;
    ++res.cases;
    {
      rex::CallFrame frame(ctx);
      PPCContext& c = frame.ctx;
      c.r1.u32 = (ctx.r1.u32 - 0x200) & ~0xFu;
      Result o;
      const int64_t t0 = NowNs();
      const Why why = Parse(base, c.r1.u32, block + kOffStr, with_end ? block + kOffEnd2 : 0,
                            uint32_t(b), flags, &o);
      if (why != kNative) continue;  // the game's code would set errno: not run
      Commit(base, o, c);
      const uint64_t r_native = c.r3.u64;
      const int64_t t1 = NowNs();
      c.r3.u64 = kLocalePtr;
      c.r4.u64 = (block + kOffStr) | junk;
      c.r5.u64 = with_end ? block + kOffEnd : 0;
      c.r6.u64 = uint64_t(int64_t(b)) ^ junk;
      c.r7.u64 = flags;
      __imp__sub_82350BA8(c, base);
      const int64_t t2 = NowNs();
      ++res.native;
      native_ns += t1 - t0;
      game_ns += t2 - t1;
      const uint32_t end_game = LdW(h + kOffEnd), end_native = LdW(h + kOffEnd2);
      const bool same = c.r3.u64 == r_native && end_game == end_native &&
                        std::memcmp(h + kOffStr, str.data(), str.size()) == 0;
      if (!same && res.same) {
        res.same = false;
        std::string shown;
        for (char ch : str) {
          if (ch >= 32 && ch < 127) {
            shown += ch;
          } else {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\x%02X", uint8_t(ch));
            shown += hex;
          }
        }
        char msg[300];
        std::snprintf(msg, sizeof(msg),
                      "\"%.80s\" base %d flags %llu: the game's code %016llx end +%d, native %016llx "
                      "end +%d",
                      shown.c_str(), b, static_cast<unsigned long long>(flags),
                      static_cast<unsigned long long>(c.r3.u64),
                      end_game ? int(end_game - block - kOffStr) : -1,
                      static_cast<unsigned long long>(r_native),
                      end_native ? int(end_native - block - kOffStr) : -1);
        res.first_diff = msg;
      }
    }
  }
  if (res.native) {
    res.game_ns = double(game_ns) / res.native;
    res.native_ns = double(native_ns) / res.native;
  }
  return res;
}

std::atomic<int> g_check_state{0};
std::atomic<bool> g_ok{false};
std::atomic<int> g_live_checks{0};
constexpr int kLiveChecks = 64;
constexpr uint32_t kSpotEvery = 1024;  // a power of two
std::atomic<int64_t> g_test_pending{0};

bool RunCheck(PPCContext& ctx, uint8_t* base, int count, bool initial) {
  auto* ks = rex::system::kernel_state();
  auto* mem = ks ? ks->memory() : nullptr;
  if (!mem) return false;
  const uint32_t alloc = mem->SystemHeapAlloc(kBlock + 0x1000);
  const uint32_t block = (alloc + 0xFFFu) & ~0xFFFu;
  if (!alloc || !Plain(base, block, kBlock)) {
    if (alloc) mem->SystemHeapFree(alloc);
    REXLOG_WARN("cod3: native strtol: no scratch memory for its check");
    return false;
  }
  const int64_t start = NowNs();
  const CheckResult r =
      RunCases(ctx, base, block, count, initial ? 0x6A09E667F3BCC909ull : uint64_t(NowNs()) | 1);
  mem->SystemHeapFree(alloc);
  const double ms = double(NowNs() - start) / 1e6;
  if (!r.same) {
    g_ok.store(false, std::memory_order_relaxed);
    g_differed.store(true, std::memory_order_relaxed);
    REXLOG_ERROR("cod3: native strtol differs from the game's own code ({}); it stays on the "
                 "game's code",
                 r.first_diff);
  } else if (initial) {
    g_ok.store(r.native > 0, std::memory_order_relaxed);
  }
  REXLOG_INFO("cod3: native strtol {} ({:.1f} ms): {} random strings, {} run natively{} (the rest "
              "would set errno: left to the game's code); the game's code {:.0f} ns a call, "
              "native {:.0f} ns{}",
              initial ? "self-check" : "test", ms, r.cases, r.native,
              r.same ? " and all the same" : ", one DIFFERED", r.game_ns, r.native_ns,
              initial ? (g_ok.load(std::memory_order_relaxed) ? "; now native (cod3_native_strtol)"
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
  REXLOG_INFO("cod3: native strtol test: {} random strings on the next atoi/strtol call (the game "
              "pauses while it runs)",
              n);
}

bool EnsureChecked(PPCContext& ctx, uint8_t* base) {
  int expected = 0;
  if (!g_check_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    return expected == 2;
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory() || ks->memory()->virtual_membase() != base ||
      !Plain(base, kLocalePtr, 4)) {
    REXLOG_WARN("cod3: native strtol off: guest memory isn't where the game's code expects it");
    g_check_state.store(2, std::memory_order_release);
    return true;
  }
  const bool ran = RunCheck(ctx, base, 4000, true);
  g_check_state.store(ran ? 2 : 0, std::memory_order_release);
  return ran;
}

inline bool Use(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cod3_native_strtol)) return false;
  if (g_check_state.load(std::memory_order_acquire) != 2 && !EnsureChecked(ctx, base))
    return false;
  if (g_test_pending.load(std::memory_order_relaxed) != 0) {
    const int64_t n = g_test_pending.exchange(0, std::memory_order_relaxed);
    if (n > 0) RunCheck(ctx, base, int(n), false);
  }
  return g_ok.load(std::memory_order_relaxed);
}

inline bool Checking(Counters& c) {
  if (g_live_checks.load(std::memory_order_relaxed) > 0 &&
      g_live_checks.fetch_sub(1, std::memory_order_relaxed) > 0)
    return true;
  if (((++c.tick) & (kSpotEvery - 1)) == 0) {
    MaybeReport();
    return true;
  }
  return REXCVAR_GET(cod3_native_strtol_check_all);
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_strtol_report, ReportCommand, "cod3/Performance",
                            "cod3_native_strtol_report: how many atoi/strtol calls ran as native "
                            "code and why the rest didn't, written to the log");
REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_strtol_test, TestCommand, "cod3/Performance",
                            "cod3_native_strtol_test N: compares N more random strings (1000 to "
                            "1000000, default 100000) with the game's code on the next atoi/strtol "
                            "call, written to the log");

// strtoxl (cod3mp sub_82320008): r4 = string, r5 = endptr, r6 = base, r7 =
// flags (r3, the locale, isn't read: the code uses the global).
REX_HOOK_RAW(sub_82350BA8) {
  if (Use(ctx, base)) {
    Counters& c = Mine();
    Result o;
    const Why why = Parse(base, ctx.r1.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u64, &o);
    if (why == kNative) {
      if (Checking(c)) {
        __imp__sub_82350BA8(ctx, base);
        Bump(c.checked);
        const uint32_t end_game = o.endptr ? LdW(Host(base, o.endptr)) : 0;
        if (ctx.r3.u64 != o.r3 || (o.endptr && end_game != o.end)) {
          g_ok.store(false, std::memory_order_relaxed);
          if (!g_differed.exchange(true)) {
            REXLOG_ERROR("cod3: native strtol differed from the game's own code in a call "
                         "(the game's code {:#018x} end {:08X}, native {:#018x} end {:08X}); the "
                         "game's code runs from now on",
                         ctx.r3.u64, end_game, o.r3, o.end);
          }
        }
        return;
      }
      Commit(base, o, ctx);
      Bump(c.native);
      return;
    }
    Bump(c.guest[why]);
  }
  __imp__sub_82350BA8(ctx, base);
}
