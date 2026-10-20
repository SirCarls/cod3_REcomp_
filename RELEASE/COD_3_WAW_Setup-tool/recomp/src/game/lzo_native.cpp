// cod3 - [native-lzo] the game's LZO1X decompressor as native code.
//
// Call of Duty 3's level archives (.cod, KAPF) keep their chunks as NCH blocks
// whose payload is a plain LZO1X stream (docs/formats/cod3.md). The game's job
// for one block (0x82125BF0, on the job workers while a level loads) checks the
// block's header and calls the decompressor, sub_821258C8: LZO's own
// lzo1x_decompress (the fast version without bounds checks), compiled to copy
// one byte at a time:
//
//   r3 = lzo1x_decompress(in r3, in_len r4, out r5, out_len_ptr r6, wrkmem r7)
//     - *out_len = 0 first, then the stream is decoded into out;
//     - at its end marker *out_len = the bytes written, and r3 = 0 when the
//       whole input was used, LZO_E_INPUT_NOT_CONSUMED (-8) when some is left,
//       LZO_E_INPUT_OVERRUN (-4) when it read past it (as the code computes
//       them: r3 is 0xFFFFFFF8 zero-extended for -8, and -4 sign-extended).
//
// This file decodes the same stream as native code, with the same instructions
// in the same order where order matters (matches copy forwards one byte after
// another, so an overlapping match repeats its pattern; the literal and match
// lengths, the offsets and the end test are computed exactly as the code does).
//
// It decodes into a buffer of its own first, so nothing in guest memory changes
// until the whole stream has been decoded: the input must be plain guest
// memory (the way the generated code reaches it, see crt_native.cpp), every
// read must stay inside the input plus a small margin, every match must look
// back inside what this call wrote, and the output must be plain guest memory
// that overlaps neither the input it read nor the length word. Anything else
// (a damaged stream above all, which the code would follow wherever it leads)
// goes to the game's own code, with guest memory as it was.
//
// The first calls (kChecks) are decoded both ways: the native result is kept
// aside, the game's code runs and writes guest memory, and its output, length
// word and r3 are compared with the native ones. A difference keeps the game's
// result and switches the native code off for the session; after kChecks equal
// calls one call in kSpotEvery is still compared (every call with
// cod3_native_lzo_check_all, for tests). cod3_native_lzo (on) switches it off
// at any time.
//
// Both programs: cod3mp has the same decompressor at 0x82137910;
// tools/mp_aliases.py points sub_821258C8 at it there.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

REXCVAR_DEFINE_BOOL(cod3_native_lzo, true, "cod3/Performance",
                    "The game's LZO1X decompressor (level archives, while a level loads) runs as "
                    "native code where that gives exactly what the game's own code does (its first "
                    "calls are checked against the game's code; off: the translated Xbox 360 code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_native_lzo_check_all, false, "cod3/Performance",
                    "Testing: every LZO1X block is decoded both ways and compared with the game's "
                    "code (loads take longer)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_821258C8);  // lzo1x_decompress (cod3mp: sub_82137910)

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
    auto* mem = rex::system::kernel_state()->memory();
    const uint32_t probe = 0xE0010000u;
    v = (mem && mem->TranslateVirtual<uint8_t*>(probe) == base + probe + kHighViewOffset) ? 1 : 0;
    g_high_view.store(v, std::memory_order_relaxed);
  }
  return v == 1;
}

inline uint8_t* Host(uint8_t* base, uint32_t addr) {
  return base + addr + (addr >= kHighView ? kHighViewOffset : 0u);
}

// A guest range [begin, end) (64-bit so nothing wraps here).
struct Range {
  uint64_t begin = 0;
  uint64_t end = 0;
};

inline bool Overlap(const Range& a, const Range& b) {
  return a.begin < b.end && b.begin < a.end;
}

bool Plain(uint8_t* base, const Range& r) {
  if (r.end <= r.begin) return true;  // empty
  if (r.end > 0x100000000ull) return false;
  if (r.begin < kMmioEnd && r.end > kMmioBegin) return false;
  if (r.end > kHighView) {
    if (r.begin < kHighView) return false;
    if (!HighViewOk(base)) return false;
  }
  return true;
}

inline uint32_t LdW(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void StW(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

constexpr uint32_t kInputMargin = 64;         // reads allowed past in_len (a damaged stream)
constexpr size_t kMaxOutput = 64u << 20;      // more output than this goes to the game's code
// Inputs above this go to the game's code: a long length's run of zero bytes
// adds 255 each, and with at most 16 MB (+ the margin) of them the 32-bit
// length the code keeps can't wrap, so the native lengths are its lengths.
constexpr uint32_t kMaxInput = 16u << 20;

enum Why : int { kOk = 0, kInput, kBehind, kLarge, kMemory, kOverlaps, kWhyCount };
constexpr const char* kWhyName[kWhyCount] = {"ok", "input", "behind", "large", "memory",
                                             "overlap"};

struct Decoded {
  size_t out_len = 0;
  size_t in_read = 0;  // one past the highest input offset read
  uint64_t r3 = 0;     // as the code leaves it
};

// Decodes the stream at in (in_len bytes, readable up to window) into out.
// Mirrors sub_821258C8 label by label (the addresses are its blocks).
Why Decode(const uint8_t* in, size_t in_len, size_t window, std::vector<uint8_t>& out,
           Decoded* d) {
  size_t ip = 0, op = 0, hi = 0;
  uint32_t t = 0;
  size_t m = 0;
  uint8_t* o = out.data();
  size_t cap = out.size();

#define COD3_NEED_IN(n)                       \
  do {                                        \
    if (ip + size_t(n) > window) return kInput; \
    hi = std::max(hi, ip + size_t(n));        \
  } while (0)
#define COD3_NEED_OUT(n)                                              \
  do {                                                                \
    const size_t need_ = op + size_t(n);                              \
    if (need_ > cap) {                                                \
      if (need_ > kMaxOutput) return kLarge;                          \
      out.resize(std::min(kMaxOutput, std::max(need_, cap * 2)));     \
      o = out.data();                                                 \
      cap = out.size();                                               \
    }                                                                 \
  } while (0)
// The back reference m = op - dist, which must land in this call's output.
#define COD3_BACK(dist)                     \
  do {                                      \
    const size_t dist_ = size_t(dist);      \
    if (dist_ > op) return kBehind;         \
    m = op - dist_;                         \
  } while (0)
// The zero run of a long length: each 0 adds 255, then the next byte and base.
#define COD3_ZERO_RUN(base_)               \
  do {                                     \
    for (;;) {                             \
      COD3_NEED_IN(1);                     \
      if (in[ip] != 0) break;              \
      ++ip;                                \
      t += 255;                            \
    }                                      \
    t += uint32_t(base_) + in[ip++];       \
  } while (0)

  // Copies len bytes forward from m (an overlapping match repeats its bytes).
  // Most literal runs and matches are a few bytes: those are copied inline
  // (a call to memcpy costs more than the copy).
  auto copy_back = [&](size_t len) {
    uint8_t* dst = o + op;
    const uint8_t* src = o + m;
    if (len >= 16 && op - m >= len) {
      std::memcpy(dst, src, len);
    } else {
      for (size_t k = 0; k < len; ++k) dst[k] = src[k];
    }
    op += len;
  };
  auto copy_literals = [&](size_t len) {
    uint8_t* dst = o + op;
    const uint8_t* src = in + ip;
    if (len >= 16) {
      std::memcpy(dst, src, len);
    } else {
      for (size_t k = 0; k < len; ++k) dst[k] = src[k];
    }
    ip += len;
    op += len;
  };

  COD3_NEED_IN(1);
  if (in[ip] > 17) {  // 0x821258D8
    t = uint32_t(in[ip++]) - 17;
    if (t < 4) goto match_next;
    COD3_NEED_IN(t);
    COD3_NEED_OUT(t);
    copy_literals(t);
    goto first_literal_run;
  }

loop:  // 0x8212591C
  COD3_NEED_IN(1);
  t = in[ip++];
  if (t >= 16) goto match;
  if (t == 0) COD3_ZERO_RUN(15);
  COD3_NEED_IN(size_t(t) + 3);
  COD3_NEED_OUT(size_t(t) + 3);
  copy_literals(size_t(t) + 3);

first_literal_run:  // 0x821259B0
  COD3_NEED_IN(1);
  t = in[ip++];
  if (t >= 16) goto match;
  COD3_NEED_IN(1);
  COD3_BACK(size_t(2049) + (t >> 2) + (size_t(in[ip]) << 2));
  ++ip;
  COD3_NEED_OUT(3);
  copy_back(3);
  goto match_done;

match:  // 0x82125A58
  if (t >= 64) {
    COD3_NEED_IN(1);
    COD3_BACK(size_t(1) + ((t >> 2) & 7) + (size_t(in[ip]) << 3));
    ++ip;
    t = (t >> 5) - 1;
  } else if (t >= 32) {  // 0x82125A88
    t &= 31;
    if (t == 0) COD3_ZERO_RUN(31);
    COD3_NEED_IN(2);
    COD3_BACK(size_t(1) + (in[ip] >> 2) + (size_t(in[ip + 1]) << 6));
    ip += 2;
  } else if (t >= 16) {  // 0x82125AF0
    const size_t far = size_t(t & 8) << 11;
    t &= 7;
    if (t == 0) COD3_ZERO_RUN(7);
    COD3_NEED_IN(2);
    const size_t near = size_t(in[ip] >> 2) + (size_t(in[ip + 1]) << 6);
    ip += 2;
    if (far + near == 0) goto eof;  // m_pos == op: the end marker
    COD3_BACK(far + near + 0x4000);
  } else {  // 0x82125BA4
    COD3_NEED_IN(1);
    COD3_BACK(size_t(1) + (t >> 2) + (size_t(in[ip]) << 2));
    ++ip;
    COD3_NEED_OUT(2);
    copy_back(2);
    goto match_done;
  }
  // copy_match, 0x82125B64: two bytes, then t more.
  COD3_NEED_OUT(size_t(t) + 2);
  copy_back(size_t(t) + 2);

match_done:  // 0x82125A00
  if (ip < 2) return kInput;  // (it reads ip[-2])
  t = in[ip - 2] & 3u;
  if (t == 0) goto loop;

match_next:  // 0x82125A10
  COD3_NEED_IN(t);
  COD3_NEED_OUT(t);
  copy_literals(t);
  COD3_NEED_IN(1);
  t = in[ip++];
  goto match;

eof:  // 0x82125BC8
  d->out_len = op;
  d->in_read = hi;
  if (ip == in_len) {
    d->r3 = 0;
  } else if (ip < in_len) {
    d->r3 = 0xFFFFFFF8ull;  // (0xFFFFFFFC & -1) - 4, zero-extended by rlwinm
  } else {
    d->r3 = uint64_t(int64_t(-4));
  }
  return kOk;

#undef COD3_NEED_IN
#undef COD3_NEED_OUT
#undef COD3_BACK
#undef COD3_ZERO_RUN
}

// ---------------------------------------------------------------------------
// The check and the counters
// ---------------------------------------------------------------------------

constexpr int kChecks = 32;              // the first calls, decoded both ways
constexpr uint64_t kSpotEvery = 256;     // after those, one call in this many too

std::mutex g_lock;  // g_checks_left and g_spot (job workers call this in parallel)
int g_checks_left = kChecks;
uint64_t g_spot = 0;
std::atomic<bool> g_off{false};
std::atomic<bool> g_announced{false};
std::atomic<uint64_t> g_native{0}, g_guest{0}, g_checked{0}, g_passed{0};
std::atomic<uint64_t> g_native_bytes{0}, g_native_ns{0};
std::atomic<uint64_t> g_guest_bytes{0}, g_guest_ns{0};
std::atomic<uint64_t> g_why[kWhyCount];
std::atomic<int64_t> g_next_report_ns{0};

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void Report() {
  std::string why;
  for (int k = 1; k < kWhyCount; ++k) {
    const uint64_t v = g_why[k].load(std::memory_order_relaxed);
    if (v) why += std::string(why.empty() ? "" : ", ") + kWhyName[k] + " " + std::to_string(v);
  }
  auto mbps = [](uint64_t bytes, uint64_t ns) {
    return ns ? double(bytes) / double(ns) * 1e9 / double(1 << 20) : 0.0;
  };
  REXLOG_INFO("cod3: native LZO1X so far: {} blocks native ({:.1f} MB out, {:.0f} MB/s), {} "
              "compared with the game's code, {}; {} by the game's code{}{} ({:.0f} MB/s)",
              g_native.load(), double(g_native_bytes.load()) / double(1 << 20),
              mbps(g_native_bytes.load(), g_native_ns.load()), g_checked.load(),
              g_off.load(std::memory_order_relaxed) ? "one differed (native off since)"
                                                    : "all the same",
              g_guest.load(), why.empty() ? "" : " (", why.empty() ? "" : why + ")",
              mbps(g_guest_bytes.load(), g_guest_ns.load()));
}

// A minute after the first block, then every 10 minutes while blocks come in
// (cod3_native_lzo_report prints it at any time).
void MaybeReport() {
  const int64_t now = int64_t(NowNs());
  int64_t next = g_next_report_ns.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_report_ns.compare_exchange_strong(next, now + 60'000'000'000ll);
    return;
  }
  if (now < next) return;
  if (!g_next_report_ns.compare_exchange_strong(next, now + 600'000'000'000ll)) return;
  Report();
}

void ReportCommand(std::string_view) { Report(); }

// The game's code, timed (bytes from its own length word).
void RunGuest(PPCContext& ctx, uint8_t* base, uint32_t out_len_ptr, bool len_ok) {
  const uint64_t t0 = NowNs();
  __imp__sub_821258C8(ctx, base);
  g_guest_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  if (len_ok) g_guest_bytes.fetch_add(LdW(Host(base, out_len_ptr)), std::memory_order_relaxed);
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_lzo_report, ReportCommand, "cod3/Performance",
                            "cod3_native_lzo_report: how many LZO1X blocks ran as native code and "
                            "how fast, against the game's code, written to the log");

REX_HOOK_RAW(sub_821258C8) {
  static thread_local std::vector<uint8_t> scratch;

  const uint32_t in = ctx.r3.u32, in_len = ctx.r4.u32, out = ctx.r5.u32, len_ptr = ctx.r6.u32;
  const Range len_word{len_ptr, uint64_t(len_ptr) + 4};
  const bool len_ok = Plain(base, len_word);

  if (!REXCVAR_GET(cod3_native_lzo) || g_off.load(std::memory_order_relaxed)) {
    g_guest.fetch_add(1, std::memory_order_relaxed);
    RunGuest(ctx, base, len_ptr, len_ok);
    MaybeReport();
    return;
  }

  // What the decoder may read: the input and a margin, plain, and not under
  // the length word (the code zeroes it before reading the input).
  Why why = kOk;
  Range window{in, uint64_t(in) + uint64_t(in_len) + kInputMargin};
  if (in_len > kMaxInput) {
    why = kLarge;
  } else if (!len_ok) {
    why = kMemory;
  } else {
    if (!Plain(base, window)) window.end = uint64_t(in) + in_len;  // no margin then
    if (!Plain(base, window)) {
      why = kMemory;
    } else if (Overlap(window, len_word)) {
      why = kOverlaps;
    }
  }
  Decoded d;
  if (why == kOk) {
    if (scratch.size() < (1u << 20)) scratch.resize(1u << 20);
    const uint64_t t0 = NowNs();
    why = Decode(Host(base, in), in_len, size_t(window.end - window.begin), scratch, &d);
    if (why == kOk) {
      const Range written{out, uint64_t(out) + d.out_len};
      const Range read{in, uint64_t(in) + d.in_read};
      if (!Plain(base, written)) {
        why = kMemory;
      } else if (Overlap(written, read) || Overlap(written, len_word)) {
        why = kOverlaps;
      }
    }
    if (why == kOk) g_native_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  }
  if (why != kOk) {
    g_why[why].fetch_add(1, std::memory_order_relaxed);
    g_guest.fetch_add(1, std::memory_order_relaxed);
    RunGuest(ctx, base, len_ptr, len_ok);
    MaybeReport();
    return;
  }

  if (!g_announced.exchange(true)) {
    REXLOG_INFO("cod3: native LZO1X on; its first {} blocks are checked against the game's code "
                "(cod3_native_lzo)",
                kChecks);
  }

  bool check = false;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_checks_left > 0) {
      --g_checks_left;
      check = true;
    } else if (++g_spot % kSpotEvery == 0 || REXCVAR_GET(cod3_native_lzo_check_all)) {
      check = true;
    }
  }
  if (check) {
    // The game's code writes guest memory; the native result is compared with it.
    RunGuest(ctx, base, len_ptr, len_ok);
    g_checked.fetch_add(1, std::memory_order_relaxed);
    const bool same = std::memcmp(Host(base, out), scratch.data(), d.out_len) == 0 &&
                      LdW(Host(base, len_ptr)) == uint32_t(d.out_len) && ctx.r3.u64 == d.r3;
    if (!same) {
      g_off.store(true, std::memory_order_relaxed);
      size_t at = 0;
      while (at < d.out_len && Host(base, out)[at] == scratch[at]) ++at;
      REXLOG_ERROR("cod3: native LZO1X differs from the game's code ({} bytes in, {} out; game: {} "
                   "out, r3 {:#x}; native r3 {:#x}; first output difference at {}); the game's "
                   "code runs it from now on",
                   in_len, d.out_len, LdW(Host(base, len_ptr)), ctx.r3.u64, d.r3, at);
    } else if (g_passed.fetch_add(1, std::memory_order_relaxed) + 1 == uint64_t(kChecks)) {
      REXLOG_INFO("cod3: native LZO1X: {} blocks the same as the game's code; native from now on",
                  kChecks);
    }
    MaybeReport();
    return;
  }

  std::memcpy(Host(base, out), scratch.data(), d.out_len);
  StW(Host(base, len_ptr), uint32_t(d.out_len));
  ctx.r3.u64 = d.r3;
  g_native.fetch_add(1, std::memory_order_relaxed);
  g_native_bytes.fetch_add(d.out_len, std::memory_order_relaxed);
  MaybeReport();
}
