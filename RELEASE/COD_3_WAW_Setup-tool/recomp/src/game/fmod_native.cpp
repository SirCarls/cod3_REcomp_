// cod3 - [native-fmod] FMOD's SFX reverb kernel as native code.
//
// FMOD Ex's "SFX Reverb" DSP (DSPSfxReverb_read, 0x82392B50) spends nearly all
// of its time in one function, sub_823A65A8: a feedback delay network. In the
// function profiles of the sound thread (XThread82387D30) it was 50-70% of the
// thread, the largest single cost of the game's sound. This file does the same
// work as native code:
//
//   reverb(state, in[], out[], channels, n), per call (DSPSfxReverb_read passes
//   the same planar buffers as in and out):
//     - a tiny alternating offset (+dc on even samples, -dc on odd ones) is
//       added to every input channel in place (keeps the filters out of
//       denormals);
//     - the channels are mixed down to mono (sub_823A60E8: a copy for one
//       channel, else the sum times 1/channels);
//     - per sample: two one-pole filters into a 100000-sample pre-delay line;
//       N taps from it, one per delay line (N = 4 or 8 here); each line's
//       output read at its index A, filtered (one pole), and the N results
//       mixed by log2(N) stages of plane rotations (x0 + c*x1, x1 - c*x0) and
//       written back into the lines, rotated by one; each output channel is the
//       sum of every channels-th line read at its index B;
//     - every index steps back by one, wrapping to its line's end.
//
// A native call does exactly what the translated code does: the same floating
// point operations in the same order (fmadds as a double fma rounded to float,
// as the generated code computes it; single adds, subtracts and multiplies as
// float operations, which round the same as the generated double-then-float
// ones), the same bytes written everywhere (the state, the lines, the buffers,
// the two argument words in the caller's frame), and the guest's own memory
// reads for every value that could change (nothing is cached across a write
// that could reach it). It runs only where every address it touches is plain
// guest memory reached the generated code's way (see crt_native.cpp), the
// buffers don't overlap the reverb's state, and the state's values are in the
// ranges the code keeps them in (N from 0 to 8, channels from 1 to 8, the
// indices inside their lines); anything else goes to the game's own code.
// N = 16 (whose arrays overrun each other in FMOD's state) always does.
//
// The first calls of each (N, channels) shape are run both ways: the game's
// code first, then the native code on the same memory as it was before, and
// every byte either one wrote is compared. The game keeps the game's result if
// anything differs (and the native code is switched off for the session); after
// kChecksPerShape equal calls that shape runs natively, and one call in
// kSpotEvery is still checked the same way for the rest of the session.
// cod3_native_fmod (on) switches it off at any time.
//
// Where the CPU has FMA (and the OS saves AVX state), the same code is built a
// second time with FMA enabled, so the fmas are single instructions instead of
// calls into the C library; both round exactly the same (fma is exact).
//
// The reverb unit's read callback around the kernel (DSPSfxReverb_read: the
// input split into one plane per channel, the kernel, the planes mixed back
// into the output with the dry signal) is native too, with the same check; see
// the end of this file. A NaN in the input sends both to the game's code for
// the rest of the session (IsNan).
//
// Both programs: cod3mp has the same kernel at 0x82375C30 (the same instructions
// apart from branch targets and where two constants sit) and the same read
// callback at 0x82362220; tools/mp_aliases.py points sub_823A65A8 and
// sub_82392B50 at them there and COD3_ADDR gives the constants' addresses.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// No <cpuid.h>: on Windows (llvm-mingw) its __cpuid macro collides with
// <intrin.h>'s function of that name, which the SDK's headers include.
#if defined(__x86_64__) || defined(_M_X64)
#define COD3_FMOD_X86 1
#else
#define COD3_FMOD_X86 0
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "game/program.h"

// No contraction of a * b + c into an fma anywhere here: the separate roundings
// are part of what the game's code computes.
#if defined(__clang__)
#pragma clang fp contract(off)
#pragma STDC FP_CONTRACT OFF
#endif

REXCVAR_DEFINE_BOOL(cod3_native_fmod, true, "cod3/Performance",
                    "FMOD's reverb (the sound's biggest cost: its kernel and its read callback) "
                    "runs as native code where that does exactly what the game's own code does "
                    "(their first calls are checked against the game's code; off: the translated "
                    "Xbox 360 code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_823A65A8);  // the SFX reverb kernel (cod3mp: sub_82375C30)

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

inline uint32_t Swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}
inline uint32_t LdW(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return Swap32(v);
}
inline void StW(uint8_t* p, uint32_t v) {
  v = Swap32(v);
  std::memcpy(p, &v, 4);
}
inline float LdF(const uint8_t* p) {
  const uint32_t v = LdW(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void StF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  StW(p, v);
}

// ---------------------------------------------------------------------------
// FMOD's reverb state (offsets from the state's address, r3)
// ---------------------------------------------------------------------------

constexpr uint32_t kMono = 0x0;          // pointer: the mono mix, n floats
constexpr uint32_t kLines = 0x4;         // pointer: the delay lines' pointers
constexpr uint32_t kInGain = 0x14;       // a
constexpr uint32_t kDamp = 0x18;         // g
constexpr uint32_t kLow1 = 0x1C;         // the first one-pole filter's output
constexpr uint32_t kLow2 = 0x20;         // the second one's
constexpr uint32_t kPre = 0x24;          // the pre-delay line, kPreLength floats
constexpr uint32_t kTapOut = 0x61AA4;    // [8] what each line's tap read
constexpr uint32_t kTapGain = 0x61AC4;   // [8]
constexpr uint32_t kOutGain = 0x61AE4;   // [8] q
constexpr uint32_t kHold = 0x61B04;      // [8] p (the line filter's feedback)
constexpr uint32_t kLineOut = 0x61B24;   // [8] each line's filtered output
constexpr uint32_t kStages = 0x61B44;    // stage s's outputs at kStages + 0x20 * s
constexpr uint32_t kLineGain = 0x61BC4;  // [8] k
constexpr uint32_t kRotate = 0x61BE4;    // c
constexpr uint32_t kTapScale = 0x61BEC;  // d1
constexpr uint32_t kLineScale = 0x61BF0; // d2
constexpr uint32_t kLineCount = 0x61C14; // N
constexpr uint32_t kLength = 0x61C18;    // [8] each line's length
constexpr uint32_t kReadOffset = 0x61C38;// [8] where index B starts from the write index
constexpr uint32_t kWrite = 0x61C58;     // [8] each line's write index
constexpr uint32_t kReadA = 0x61C78;     // [8]
constexpr uint32_t kReadB = 0x61C98;     // [8]
constexpr uint32_t kTapIndex = 0x61CB8;  // [8] the taps' indices into the pre-delay line
constexpr uint32_t kPreWrite = 0x61CD8;  // the pre-delay line's write index
constexpr uint32_t kOwner = 0x61D00;     // pointer: an object whose +8 and +2222 the call clears
constexpr uint32_t kDone = 0x61D04;      // 0 while the call runs after a reset, 1 after it
constexpr uint32_t kStateSize = 0x61D08;
constexpr int32_t kPreLength = 100000;   // the pre-delay indices wrap to 99999

// The game's own constants the code reads (read-only data; cod3mp's copies sit
// elsewhere, mp/config/campaign_map.tsv). kZeroAddr follows kOneAddr in both.
constexpr uint32_t kDcAddr = COD3_ADDR(0x82060018, 0x8205EA40);    // the alternating input offset
constexpr uint32_t kOneAddr = COD3_ADDR(0x820014D0, 0x82001570);   // 1.0, divided by the channel count
constexpr uint32_t kZeroAddr = COD3_ADDR(0x820014D4, 0x82001574);  // 0.0, where sums start
static_assert(kZeroAddr == kOneAddr + 4, "Prepare() checks them as one range");

constexpr int kMaxLines = 8;
constexpr int kMaxChannels = 8;
constexpr uint32_t kMaxSamples = 1u << 16;
constexpr int32_t kMaxLength = 1 << 22;

#if defined(__clang__) || defined(__GNUC__)
#define COD3_FMOD_INLINE inline __attribute__((always_inline))
#else
#define COD3_FMOD_INLINE inline
#endif

// The generated code's fmadds and fnmsubs: a double fma of the three float
// values, then rounded to float.
COD3_FMOD_INLINE float Fma(float a, float b, float c) {  // a * b + c
  return float(std::fma(double(a), double(b), double(c)));
}
COD3_FMOD_INLINE float Fnms(float a, float b, float c) {  // -(a * b - c)
  return float(-std::fma(double(a), double(b), -double(c)));
}

// ---------------------------------------------------------------------------
// One call, checked and laid out
// ---------------------------------------------------------------------------

struct Call {
  uint32_t state = 0, in = 0, out = 0, sp = 0;
  int32_t channels = 0;
  uint32_t n = 0;
  int32_t lines = 0;  // N
  int terms = 0;      // lines summed per output channel
  int used = 0;       // lines touched (N, or more for the outputs)
  uint8_t* st = nullptr;
  uint8_t* in_buf[kMaxChannels] = {};
  uint8_t* out_buf[kMaxChannels] = {};
  uint8_t* mono = nullptr;
  uint8_t* owner = nullptr;
  uint8_t* line[kMaxLines] = {};  // host address of index 0 of each line
  uint8_t* frame = nullptr;       // the caller's frame at sp + 36
  int32_t length[kMaxLines] = {};
  std::vector<Range> writes;      // every guest range the call writes (for the check)
};

enum Why : int {
  kOk = 0,
  kShape,     // N, channels or n outside what the native code takes
  kMemory,    // an address that isn't plain guest memory
  kIndex,     // an index outside its line
  kOverlaps,  // a buffer over the state or over a pointer table
  kNan,       // a NaN in the input (see IsNan)
  kWhyCount
};
constexpr const char* kWhyName[kWhyCount] = {"ok", "shape", "memory", "index", "overlap", "nan"};

// A float's bits are a NaN. The native code computes every value the game's
// code does, but which NaN an operation passes on when two of its operands are
// NaNs depends on the instruction the compiler picked, so the two can't be
// matched bit for bit once a NaN with a payload of its own gets in. NaNs made
// by the arithmetic itself (inf - inf ...) are the CPU's one default NaN, the
// same either way. So a NaN in the input sends that call, and every later one,
// to the game's code.
inline bool IsNan(uint32_t bits) {
  return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0;
}
bool AnyNan(const uint8_t* p, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v;
    std::memcpy(&v, p + 4 * i, 4);
    if (IsNan(Swap32(v))) return true;
  }
  return false;
}

// The range an index can reach in a call: it is used, then steps back by one
// and wraps to length - 1 below 0.
inline void Reach(int32_t start, int32_t length, int64_t* lo, int64_t* hi) {
  *lo = std::min<int64_t>(start, 0);
  *hi = std::max<int64_t>(start, int64_t(length) - 1);
}

// The kernel's arguments as the code reads them: r3 state, r4 / r5 the input
// and output pointer tables, r6 channels (signed), r7 samples, r1 the stack.
// scan_inputs: look for NaNs in the input channels (off when the caller is
// about to fill them, see ReadPrepare).
Why PrepareArgs(uint8_t* base, uint32_t state_addr, uint32_t in, uint32_t out, int32_t channels,
                uint32_t n, uint32_t sp, bool scan_inputs, Call* c) {
  c->state = state_addr;
  c->in = in;
  c->out = out;
  c->channels = channels;
  c->n = n;
  c->sp = sp;
  c->writes.clear();
  if (c->channels < 1 || c->channels > kMaxChannels || c->n > kMaxSamples) return kShape;

  const Range state{c->state, uint64_t(c->state) + kStateSize};
  const Range in_table{c->in, uint64_t(c->in) + 4u * c->channels};
  const Range out_table{c->out, uint64_t(c->out) + 4u * c->channels};
  const Range frame{uint64_t(c->sp) + 36, uint64_t(c->sp) + 48};
  for (const Range& r : {state, in_table, out_table, frame,
                         Range{kDcAddr, kDcAddr + 4ull}, Range{kOneAddr, kZeroAddr + 4ull}}) {
    if (!Plain(base, r)) return kMemory;
  }
  c->st = Host(base, c->state);
  c->frame = Host(base, c->sp + 36);

  c->lines = int32_t(LdW(c->st + kLineCount));
  if (c->lines < 0 || c->lines > kMaxLines) return kShape;
  c->terms = 8 / c->channels;
  c->used = std::max<int>(c->lines, c->terms * c->channels);

  const uint64_t bytes = 4ull * c->n;
  std::vector<Range>& w = c->writes;
  w.push_back(state);
  w.push_back(Range{uint64_t(c->sp) + 36, uint64_t(c->sp) + 40});
  w.push_back(Range{uint64_t(c->sp) + 44, uint64_t(c->sp) + 48});
  std::vector<Range> tables{in_table, out_table};
  Range mono_range{};

  for (int ch = 0; ch < c->channels; ++ch) {
    const uint32_t ia = LdW(Host(base, c->in) + 4 * ch);
    const uint32_t oa = LdW(Host(base, c->out) + 4 * ch);
    const Range ir{ia, ia + bytes}, orr{oa, oa + bytes};
    if (!Plain(base, ir) || !Plain(base, orr)) return kMemory;
    c->in_buf[ch] = Host(base, ia);
    c->out_buf[ch] = Host(base, oa);
    w.push_back(ir);
    w.push_back(orr);
  }
  if (scan_inputs) {
    for (int ch = 0; ch < c->channels; ++ch) {
      if (AnyNan(c->in_buf[ch], c->n)) return kNan;
    }
  }
  {
    const uint32_t ma = LdW(c->st + kMono);
    mono_range = Range{ma, ma + bytes};
    if (!Plain(base, mono_range)) return kMemory;
    c->mono = Host(base, ma);
    w.push_back(mono_range);
    // The mix sums into registers: the mono line mustn't be any input (one
    // channel is a memmove, which may overlap).
    if (c->channels > 1) {
      for (int ch = 0; ch < c->channels; ++ch) {
        const uint32_t ia = LdW(Host(base, c->in) + 4 * ch);
        if (Overlap(mono_range, Range{ia, ia + bytes})) return kOverlaps;
      }
    }
  }
  {
    const uint32_t oa = LdW(c->st + kOwner);
    const Range a{uint64_t(oa) + 8, uint64_t(oa) + 10}, b{uint64_t(oa) + 2222, uint64_t(oa) + 2223};
    if (!Plain(base, a) || !Plain(base, b)) return kMemory;
    c->owner = Host(base, oa);
    w.push_back(a);
    w.push_back(b);
  }

  // The pre-delay indices.
  const int32_t pre_write = int32_t(LdW(c->st + kPreWrite));
  if (pre_write < 0 || pre_write >= kPreLength) return kIndex;
  for (int k = 0; k < c->lines; ++k) {
    const int32_t t = int32_t(LdW(c->st + kTapIndex + 4 * k));
    if (t < 0 || t >= kPreLength) return kIndex;
  }

  // The lines, with A and B as the call's first loop sets them.
  const uint32_t lines_table = LdW(c->st + kLines);
  const Range lt{lines_table, uint64_t(lines_table) + 4ull * kMaxLines};
  if (!Plain(base, lt)) return kMemory;
  tables.push_back(lt);
  for (int j = 0; j < c->used; ++j) {
    const int32_t len = int32_t(LdW(c->st + kLength + 4 * j));
    if (len < 1 || len > kMaxLength) return kIndex;
    c->length[j] = len;
    const uint32_t la = LdW(Host(base, lines_table) + 4 * j);
    int64_t lo = 0, hi = -1;
    auto take = [&](int32_t idx) {
      if (idx < -kMaxLength || idx > kMaxLength) return false;
      int64_t l, h;
      Reach(idx, len, &l, &h);
      if (hi < lo) {
        lo = l;
        hi = h;
      } else {
        lo = std::min(lo, l);
        hi = std::max(hi, h);
      }
      return true;
    };
    bool ok = true;
    int64_t wlo = 0, whi = -1;
    if (j < c->lines) {
      const int32_t pos = int32_t(LdW(c->st + kWrite + 4 * j));
      int32_t a = int32_t(uint32_t(len) + uint32_t(pos) - 1u);
      if (a > len) a = int32_t(uint32_t(a) - uint32_t(len));
      int32_t b = int32_t(LdW(c->st + kReadOffset + 4 * j) + uint32_t(pos) - 1u);
      if (b > len) b = int32_t(uint32_t(b) - uint32_t(len));
      ok = take(pos) && take(a) && take(b);
      if (pos < -kMaxLength || pos > kMaxLength) ok = false;
      if (ok) Reach(pos, len, &wlo, &whi);
    } else {
      ok = take(int32_t(LdW(c->st + kReadB + 4 * j)));
    }
    if (!ok) return kIndex;
    const Range r{uint64_t(int64_t(la) + 4 * lo), uint64_t(int64_t(la) + 4 * (hi + 1))};
    if (int64_t(la) + 4 * lo < 0 || !Plain(base, r)) return kMemory;
    if (Overlap(r, state)) return kOverlaps;
    c->line[j] = Host(base, la);  // index 0; r is plain, so Host() is uniform over it
    if (whi >= wlo && (c->lines == 4 || c->lines == 8)) {
      w.push_back(Range{uint64_t(int64_t(la) + 4 * wlo), uint64_t(int64_t(la) + 4 * (whi + 1))});
    }
  }

  // Nothing the call writes may sit on the state (it is kept in registers
  // between its own accesses) or on a pointer table (read once).
  for (size_t i = 1; i < w.size(); ++i) {
    if (Overlap(w[i], state)) return kOverlaps;
    for (const Range& t : tables) {
      if (Overlap(w[i], t)) return kOverlaps;
    }
  }
  for (const Range& t : tables) {
    if (Overlap(t, state)) return kOverlaps;
  }
  return kOk;
}

Why Prepare(PPCContext& ctx, uint8_t* base, Call* c) {
  return PrepareArgs(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.s32, ctx.r7.u32,
                     ctx.r1.u32, true, c);
}

// ---------------------------------------------------------------------------
// The kernel
// ---------------------------------------------------------------------------

// One rotation stage over x (in place): pairs h apart in groups of 2h.
COD3_FMOD_INLINE void Stage(float* x, int count, int h, float c) {
  for (int g = 0; g < count; g += 2 * h) {
    for (int j = 0; j < h; ++j) {
      const float a = x[g + j], b = x[g + j + h];
      x[g + j] = Fma(b, c, a);
      x[g + j + h] = Fnms(a, c, b);
    }
  }
}

// N: the line count when it is 4 or 8 (the rotation network runs), known at
// compile time so the per-line loops unroll; 0 for any other count (0 to 7,
// no network). Everything the loop needs is copied into locals first: the
// guest memory writes go through byte pointers, which the compiler must
// otherwise assume could change anything reachable.
template <int N>
COD3_FMOD_INLINE void Kernel(const Call& c, uint8_t* base) {
  uint8_t* const st = c.st;
  const int channels = c.channels;
  const uint32_t n = c.n;
  const int lines = N ? N : c.lines;
  constexpr bool kMix = N == 4 || N == 8;
  constexpr int kStageCount = N == 8 ? 3 : 2;
  const int terms = c.terms;
  const int used = c.used;
  uint8_t* line[kMaxLines];
  uint8_t* in_buf[kMaxChannels];
  uint8_t* out_buf[kMaxChannels];
  for (int j = 0; j < kMaxLines; ++j) line[j] = c.line[j];
  for (int ch = 0; ch < kMaxChannels; ++ch) {
    in_buf[ch] = c.in_buf[ch];
    out_buf[ch] = c.out_buf[ch];
  }
  uint8_t* const mono = c.mono;
  uint8_t* const owner = c.owner;

  // The argument words the code keeps in its caller's frame (r5, r6).
  StW(c.frame, c.out);
  StW(c.frame + 8, uint32_t(channels));

  // The alternating offset, in place on every input channel.
  const float dc = LdF(Host(base, kDcAddr));
  for (int ch = 0; ch < channels; ++ch) {
    uint8_t* p = in_buf[ch];
    for (uint32_t i = 0; i < n; i += 2) StF(p + 4 * i, LdF(p + 4 * i) + dc);
    for (uint32_t i = 1; i < n; i += 2) StF(p + 4 * i, LdF(p + 4 * i) - dc);
  }

  // The mono mix (sub_823A60E8).
  if (channels == 1) {
    std::memmove(mono, in_buf[0], size_t(n) * 4);
  } else if (n != 0) {
    const float inv = LdF(Host(base, kOneAddr)) / float(channels);
    const float zero = LdF(Host(base, kZeroAddr));
    for (uint32_t i = 0; i < n; ++i) {
      float acc = zero;
      for (int ch = 0; ch < channels; ++ch) acc = LdF(in_buf[ch] + 4 * i) + acc;
      StF(mono + 4 * i, inv * acc);
    }
  }

  // The owner object's flags.
  owner[2222] = 0;
  if (((uint32_t(owner[8]) << 8) | owner[9]) == 1) {
    owner[8] = 0;
    owner[9] = 0;
    StW(st + kDone, 0);
  }

  // Each line's read indices from its write index.
  int32_t len[kMaxLines] = {}, pos[kMaxLines] = {}, ra[kMaxLines] = {}, rb[kMaxLines] = {};
  for (int i = 0; i < lines; ++i) {
    len[i] = int32_t(LdW(st + kLength + 4 * i));
    pos[i] = int32_t(LdW(st + kWrite + 4 * i));
    int32_t a = int32_t(uint32_t(len[i]) + uint32_t(pos[i]) - 1u);
    if (a > len[i]) a = int32_t(uint32_t(a) - uint32_t(len[i]));
    StW(st + kReadA + 4 * i, uint32_t(a));
    ra[i] = a;
    int32_t b = int32_t(LdW(st + kReadOffset + 4 * i) + uint32_t(pos[i]) - 1u);
    if (b > len[i]) b = int32_t(uint32_t(b) - uint32_t(len[i]));
    StW(st + kReadB + 4 * i, uint32_t(b));
    rb[i] = b;
  }

  if (n != 0) {
    const float zero = LdF(Host(base, kZeroAddr));
    const float in_gain = LdF(st + kInGain), damp = LdF(st + kDamp);
    float low1 = LdF(st + kLow1), low2 = LdF(st + kLow2);
    float tap_gain[kMaxLines] = {}, out_gain[kMaxLines] = {}, hold[kMaxLines] = {},
          line_gain[kMaxLines] = {}, line_out[kMaxLines] = {};
    for (int k = 0; k < lines; ++k) {
      tap_gain[k] = LdF(st + kTapGain + 4 * k);
      out_gain[k] = LdF(st + kOutGain + 4 * k);
      hold[k] = LdF(st + kHold + 4 * k);
      line_gain[k] = LdF(st + kLineGain + 4 * k);
      line_out[k] = LdF(st + kLineOut + 4 * k);
    }
    const float rotate = LdF(st + kRotate), tap_scale = LdF(st + kTapScale),
                line_scale = LdF(st + kLineScale);
    int32_t pre_write = int32_t(LdW(st + kPreWrite));
    int32_t tap_index[8];
    for (int k = 0; k < 8; ++k) tap_index[k] = int32_t(LdW(st + kTapIndex + 4 * k));
    // Lines the outputs read beyond N: their B stays put in the call.
    int32_t out_b[kMaxLines] = {};
    for (int j = 0; j < used; ++j) out_b[j] = j < lines ? rb[j] : int32_t(LdW(st + kReadB + 4 * j));
    // The lines each output channel sums, in order.
    int sum_line[kMaxChannels][8] = {};
    for (int ch = 0; ch < channels; ++ch)
      for (int m = 0; m < terms; ++m) sum_line[ch][m] = ch + m * channels;

    // The state's own values (filters, taps, stage outputs, indices) are
    // written once after the loop: nothing the call writes reaches the state
    // (Prepare), so only their final values can be seen. The pre-delay line
    // is written every sample (the taps read it).
    float tap[kMaxLines] = {};
    float stage[3][kMaxLines] = {};
    for (uint32_t i = 0; i < n; ++i) {
      // Two one-pole filters into the pre-delay line.
      low1 = Fma(LdF(mono + 4 * i), in_gain, damp * low1);
      low2 = Fma(damp, low2, in_gain * low1);
      StF(st + kPre + 4 * uint32_t(pre_write), low2);

      for (int k = 0; k < lines; ++k) tap[k] = LdF(st + kPre + 4 * uint32_t(tap_index[k]));
      for (int k = 0; k < lines; ++k) {
        float t = (tap[k] * tap_gain[k]) * tap_scale;
        t = Fma(LdF(line[k] + 4 * ra[k]), line_gain[k], t);
        line_out[k] = Fma(t * line_scale, out_gain[k], hold[k] * line_out[k]);
      }

      if constexpr (kMix) {
        float x[N];
        for (int k = 0; k < N; ++k) x[k] = line_out[k];
        for (int s = 0, h = 1; s < kStageCount; ++s, h *= 2) {
          Stage(x, N, h, rotate);
          for (int k = 0; k < N; ++k) stage[s][k] = x[k];
        }
        for (int k = 0; k < N; ++k) StF(line[k] + 4 * pos[k], x[(k + 1) % N]);
      }

      for (int ch = 0; ch < channels; ++ch) {
        float acc = zero;
        for (int m = 0; m < terms; ++m) {
          const int j = sum_line[ch][m];
          acc = LdF(line[j] + 4 * out_b[j]) + acc;
        }
        StF(out_buf[ch] + 4 * i, acc);
      }

      // Every index one step back.
      if (--pre_write < 0) pre_write = kPreLength - 1;
      for (int k = 0; k < 8; ++k) {
        if (--tap_index[k] < 0) tap_index[k] = kPreLength - 1;
      }
      for (int k = 0; k < lines; ++k) {
        if (--pos[k] < 0) pos[k] = len[k] - 1;
        if (--ra[k] < 0) ra[k] = len[k] - 1;
        if (--rb[k] < 0) rb[k] = len[k] - 1;
        out_b[k] = rb[k];
      }
    }

    StF(st + kLow1, low1);
    StF(st + kLow2, low2);
    for (int k = 0; k < lines; ++k) {
      StF(st + kTapOut + 4 * k, tap[k]);
      StF(st + kLineOut + 4 * k, line_out[k]);
    }
    if constexpr (kMix) {
      for (int s = 0; s < kStageCount; ++s)
        for (int k = 0; k < N; ++k) StF(st + kStages + 0x20 * s + 4 * k, stage[s][k]);
    }
    StW(st + kPreWrite, uint32_t(pre_write));
    for (int k = 0; k < 8; ++k) StW(st + kTapIndex + 4 * k, uint32_t(tap_index[k]));
    for (int k = 0; k < lines; ++k) {
      StW(st + kWrite + 4 * k, uint32_t(pos[k]));
      StW(st + kReadA + 4 * k, uint32_t(ra[k]));
      StW(st + kReadB + 4 * k, uint32_t(rb[k]));
    }
  }
  StW(st + kDone, 1);
}

COD3_FMOD_INLINE void KernelFor(const Call& c, uint8_t* base) {
  switch (c.lines) {
    case 8: Kernel<8>(c, base); break;
    case 4: Kernel<4>(c, base); break;
    default: Kernel<0>(c, base); break;
  }
}

void RunPlain(const Call& c, uint8_t* base) { KernelFor(c, base); }

#if COD3_FMOD_X86 && (defined(__clang__) || defined(__GNUC__))
__attribute__((target("fma"))) void RunFma(const Call& c, uint8_t* base) { KernelFor(c, base); }

void Cpuid(uint32_t leaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(0u));
}

bool CpuHasFma() {
  uint32_t a = 0, b = 0, cx = 0, d = 0;
  Cpuid(0, a, b, cx, d);
  if (a < 1) return false;  // no leaf 1
  Cpuid(1, a, b, cx, d);
  const bool fma = (cx >> 12) & 1, osxsave = (cx >> 27) & 1, avx = (cx >> 28) & 1;
  if (!fma || !osxsave || !avx) return false;
  uint32_t lo = 0, hi = 0;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (lo & 6u) == 6u;  // the OS saves the XMM and YMM state
}
#else
void RunFma(const Call& c, uint8_t* base) { KernelFor(c, base); }
bool CpuHasFma() { return false; }
#endif

using Runner = void (*)(const Call&, uint8_t*);
Runner PickRunner() { return CpuHasFma() ? RunFma : RunPlain; }

// ---------------------------------------------------------------------------
// The check and the counters
// ---------------------------------------------------------------------------

constexpr int kChecksPerShape = 16;
constexpr uint64_t kSpotEvery = 2048;  // after those, one call in this many is checked too

std::mutex g_lock;  // the check's state (the sound thread calls this alone)
std::atomic<bool> g_off{false};             // a difference was seen
std::atomic<bool> g_nan{false};             // a NaN came in (IsNan)
int g_checks_left[kMaxLines + 1][kMaxChannels + 1];  // per (N, channels)
bool g_checks_init = false;
std::atomic<uint64_t> g_native{0}, g_guest{0}, g_checked{0};
// Time spent per call, for the report: native calls, and the game's code (its
// own calls and the first half of each check).
std::atomic<uint64_t> g_native_ns{0}, g_guest_ns{0}, g_guest_timed{0};
uint64_t g_spot = 0;

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
std::atomic<uint64_t> g_why[kWhyCount];

// The same for the reverb unit's read callback (DSPSfxReverb_read, below).
enum ReadWhy : int { kROk = 0, kRShape, kRMemory, kROverlaps, kRNan, kRKernel, kRWhyCount };
constexpr const char* kReadWhyName[kRWhyCount] = {"ok", "shape", "memory", "overlap", "nan",
                                                  "kernel"};
std::atomic<bool> g_read_off{false};  // a difference was seen
std::atomic<uint64_t> g_read_native{0}, g_read_guest{0}, g_read_checked{0};
std::atomic<uint64_t> g_read_native_ns{0}, g_read_guest_ns{0}, g_read_guest_timed{0};
std::atomic<uint64_t> g_read_why[kRWhyCount];
std::chrono::steady_clock::time_point g_next_report{};
bool g_reported_fma = false;

int& ChecksLeft(int lines, int channels) {
  if (!g_checks_init) {
    for (auto& row : g_checks_left)
      for (int& v : row) v = kChecksPerShape;
    g_checks_init = true;
  }
  return g_checks_left[lines][channels];
}

void CopyOut(uint8_t* base, const std::vector<Range>& rs, std::vector<uint8_t>* out) {
  size_t total = 0;
  for (const Range& r : rs) total += size_t(r.end - r.begin);
  out->resize(total);
  size_t at = 0;
  for (const Range& r : rs) {
    const size_t len = size_t(r.end - r.begin);
    std::memcpy(out->data() + at, Host(base, uint32_t(r.begin)), len);
    at += len;
  }
}

void CopyIn(uint8_t* base, const std::vector<Range>& rs, const std::vector<uint8_t>& in) {
  size_t at = 0;
  for (const Range& r : rs) {
    const size_t len = size_t(r.end - r.begin);
    std::memcpy(Host(base, uint32_t(r.begin)), in.data() + at, len);
    at += len;
  }
}

// Where the first byte that differs is, as "range i + offset".
std::string FirstDifference(const std::vector<Range>& rs, const std::vector<uint8_t>& a,
                            const std::vector<uint8_t>& b) {
  size_t at = 0;
  for (size_t i = 0; i < rs.size(); ++i) {
    const size_t len = size_t(rs[i].end - rs[i].begin);
    for (size_t k = 0; k < len; ++k) {
      if (a[at + k] != b[at + k]) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "range %zu (guest 0x%08X) + 0x%zX", i,
                      uint32_t(rs[i].begin), k);
        return buf;
      }
    }
    at += len;
  }
  return "nowhere";
}

void MaybeReport() {
  const auto now = std::chrono::steady_clock::now();
  if (now < g_next_report) return;
  const bool first = g_next_report == std::chrono::steady_clock::time_point{};
  g_next_report = now + (first ? std::chrono::seconds(60) : std::chrono::minutes(10));
  if (first) return;
  std::string why;
  for (int k = 1; k < kWhyCount; ++k) {
    const uint64_t v = g_why[k].load(std::memory_order_relaxed);
    if (v) why += std::string(why.empty() ? "" : ", ") + kWhyName[k] + " " + std::to_string(v);
  }
  const uint64_t native = g_native.load(), timed = g_guest_timed.load();
  const double native_us = native ? double(g_native_ns.load()) / 1000.0 / double(native) : 0.0;
  const double guest_us = timed ? double(g_guest_ns.load()) / 1000.0 / double(timed) : 0.0;
  REXLOG_INFO("cod3: native FMOD reverb so far: {} calls native ({:.1f} us each), {} compared with the "
              "game's code, {}; {} by the game's code{}{}; the game's code took {:.1f} us a call",
              native, native_us, g_checked.load(),
              g_off.load(std::memory_order_relaxed)   ? "one differed (native off since)"
              : g_nan.load(std::memory_order_relaxed) ? "all the same (a NaN came in: native off since)"
                                                      : "all the same",
              g_guest.load(), why.empty() ? "" : " (", why.empty() ? "" : why + ")", guest_us);

  std::string rwhy;
  for (int k = 1; k < kRWhyCount; ++k) {
    const uint64_t v = g_read_why[k].load(std::memory_order_relaxed);
    if (v) rwhy += std::string(rwhy.empty() ? "" : ", ") + kReadWhyName[k] + " " + std::to_string(v);
  }
  const uint64_t rnative = g_read_native.load(), rtimed = g_read_guest_timed.load();
  const double rnative_us = rnative ? double(g_read_native_ns.load()) / 1000.0 / double(rnative) : 0.0;
  const double rguest_us = rtimed ? double(g_read_guest_ns.load()) / 1000.0 / double(rtimed) : 0.0;
  REXLOG_INFO("cod3: native FMOD reverb unit so far (its buffers in and out, the kernel included): "
              "{} calls native ({:.1f} us each), {} compared with the game's code, {}; {} by the "
              "game's code{}{}; the game's code took {:.1f} us a call",
              rnative, rnative_us, g_read_checked.load(),
              g_read_off.load(std::memory_order_relaxed) ? "one differed (native off since)"
                                                         : "all the same",
              g_read_guest.load(), rwhy.empty() ? "" : " (", rwhy.empty() ? "" : rwhy + ")",
              rguest_us);
}

}  // namespace

// The reverb kernel: native where Prepare() allows, the first calls of each
// shape checked against the game's code, else the game's code.
REX_HOOK_RAW(sub_823A65A8) {
  static thread_local Call call;
  static thread_local std::vector<uint8_t> before, guest, native;
  static const Runner run = PickRunner();

  if (!REXCVAR_GET(cod3_native_fmod) || g_off.load(std::memory_order_relaxed) ||
      g_nan.load(std::memory_order_relaxed)) {
    const uint64_t t0 = NowNs();
    __imp__sub_823A65A8(ctx, base);
    g_guest_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    g_guest_timed.fetch_add(1, std::memory_order_relaxed);
    if ((g_guest_timed.load(std::memory_order_relaxed) & 0x3FF) == 0) {
      std::lock_guard<std::mutex> relock(g_lock);
      MaybeReport();
    }
    return;
  }
  const Why why = Prepare(ctx, base, &call);
  if (why != kOk) {
    g_why[why].fetch_add(1, std::memory_order_relaxed);
    g_guest.fetch_add(1, std::memory_order_relaxed);
    if (why == kNan && !g_nan.exchange(true)) {
      REXLOG_WARN("cod3: a NaN reached FMOD's reverb ({} channels, {} samples); the game's code "
                  "runs it from now on (NaNs can't be matched bit for bit)",
                  call.channels, call.n);
    }
    __imp__sub_823A65A8(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();  // as the game's code does before its float math

  std::unique_lock<std::mutex> lock(g_lock);
  if (!g_reported_fma) {
    g_reported_fma = true;
    REXLOG_INFO("cod3: native FMOD reverb on ({}); its first {} calls of each shape are checked "
                "against the game's code (cod3_native_fmod)",
                run == RunFma ? "with the CPU's fma" : "fma through the C library", kChecksPerShape);
  }
  int& left = ChecksLeft(call.lines, call.channels);
  if (left > 0 || ++g_spot % kSpotEvery == 0) {
    CopyOut(base, call.writes, &before);
    const uint64_t t0 = NowNs();
    __imp__sub_823A65A8(ctx, base);  // ctx ends as the game's code leaves it
    g_guest_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    g_guest_timed.fetch_add(1, std::memory_order_relaxed);
    CopyOut(base, call.writes, &guest);
    CopyIn(base, call.writes, before);
    run(call, base);
    CopyOut(base, call.writes, &native);
    g_checked.fetch_add(1, std::memory_order_relaxed);
    if (native != guest) {
      CopyIn(base, call.writes, guest);  // the game keeps the game's result
      g_off.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("cod3: native FMOD reverb differs from the game's code ({} lines, {} channels, {} "
                   "samples; first at {}); the game's code runs it from now on",
                   call.lines, call.channels, call.n, FirstDifference(call.writes, guest, native));
      return;
    }
    if (left > 0 && --left == 0) {
      REXLOG_INFO("cod3: native FMOD reverb ({} lines, {} channels): {} calls the same as the "
                  "game's code; native from now on",
                  call.lines, call.channels, kChecksPerShape);
    }
    MaybeReport();
    return;
  }
  lock.unlock();
  const uint64_t t0 = NowNs();
  run(call, base);
  g_native_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  g_native.fetch_add(1, std::memory_order_relaxed);
  if ((g_native.load(std::memory_order_relaxed) & 0x3FF) == 0) {
    std::lock_guard<std::mutex> relock(g_lock);
    MaybeReport();
  }
}

// ---------------------------------------------------------------------------
// The reverb unit's read callback: DSPSfxReverb_read (0x82392B50)
// ---------------------------------------------------------------------------
//
// read(dsp r3, in r4, out r5, length r6, channels r7), in and out interleaved
// (the same 177 instructions in cod3mp at 0x82362220):
//   - each channel of in is copied into its own plane (the planes' pointers sit
//     at dsp + 0x8A18), lfs / stfs pairs;
//   - the kernel above runs on the planes in place:
//     sub_823A65A8(*(dsp + 0xA10), planes, planes, channels, length), with f1
//     the word at dsp + 0xA14 as a float (the kernel doesn't read it);
//   - out = in * dry + plane, per sample and channel (fmadds; dry is the float
//     at dsp + 0x154, which the code reloads for every sample);
//   - r3 = 0.
// The native version does the same in the same order and calls the kernel
// through its hook (so the kernel is native or the game's by its own rules).
// It runs only where every address is plain guest memory and nothing the
// code writes changes what it reads later (the planes over each other, the
// input, the pointer table or dsp's fields; the output over any of those
// except the input itself, exactly); lfs / stfs copies are bit copies apart
// from NaNs, so a NaN in the input goes to the game's code too.

REX_EXTERN(__imp__sub_82392B50);  // DSPSfxReverb_read (cod3mp: sub_82362220)

namespace {

constexpr uint32_t kReadPlanes = 0x8A18;  // dsp: the planes' pointers, one per channel
constexpr uint32_t kReadState = 0xA10;    // dsp: the kernel's state (r3)
constexpr uint32_t kReadRate = 0xA14;     // dsp: a word passed as f1
constexpr uint32_t kReadDry = 0x154;      // dsp: the dry gain

struct ReadCall {
  uint32_t dsp = 0, in = 0, out = 0, n = 0, table = 0;
  int32_t channels = 0;
  uint64_t r1 = 0, r3 = 0, r6 = 0, r7 = 0;  // as the call got them
  uint8_t* in_h = nullptr;
  uint8_t* out_h = nullptr;
  uint8_t* plane[kMaxChannels] = {};
  std::vector<Range> writes;  // the planes, the output and the kernel's writes
};

ReadWhy ReadPrepare(PPCContext& ctx, uint8_t* base, ReadCall* r, Call* kernel) {
  r->dsp = ctx.r3.u32;
  r->in = ctx.r4.u32;
  r->out = ctx.r5.u32;
  r->n = ctx.r6.u32;
  r->channels = ctx.r7.s32;
  r->r1 = ctx.r1.u64;
  r->r3 = ctx.r3.u64;
  r->r6 = ctx.r6.u64;
  r->r7 = ctx.r7.u64;
  r->writes.clear();
  if (r->channels < 1 || r->channels > kMaxChannels) return kRShape;
  if (ctx.r6.s32 < 1 || r->n > kMaxSamples) return kRShape;

  const uint64_t channels = uint64_t(r->channels), n = r->n, dsp = r->dsp;
  const uint64_t bytes = 4 * n * channels;
  const Range table{dsp + kReadPlanes, dsp + kReadPlanes + 4 * channels};
  const Range fields{dsp + kReadState, dsp + kReadRate + 4};
  const Range dry{dsp + kReadDry, dsp + kReadDry + 4};
  const Range in{r->in, uint64_t(r->in) + bytes}, out{r->out, uint64_t(r->out) + bytes};
  for (const Range& g : {table, fields, dry, in, out}) {
    if (!Plain(base, g)) return kRMemory;
  }
  r->table = uint32_t(table.begin);
  r->in_h = Host(base, r->in);
  r->out_h = Host(base, r->out);

  Range planes[kMaxChannels];
  for (int c = 0; c < r->channels; ++c) {
    const uint32_t pa = LdW(Host(base, r->table) + 4 * c);
    planes[c] = Range{pa, uint64_t(pa) + 4 * n};
    if (!Plain(base, planes[c])) return kRMemory;
    r->plane[c] = Host(base, pa);
  }
  for (int c = 0; c < r->channels; ++c) {
    for (int d = 0; d < c; ++d) {
      if (Overlap(planes[c], planes[d])) return kROverlaps;
    }
    for (const Range& g : {in, out, table, fields, dry}) {
      if (Overlap(planes[c], g)) return kROverlaps;
    }
  }
  for (const Range& g : {table, fields, dry}) {
    if (Overlap(out, g)) return kROverlaps;
  }
  if (Overlap(out, in) && r->out != r->in) return kROverlaps;

  if (AnyNan(r->in_h, uint32_t(n * channels)) || AnyNan(Host(base, uint32_t(dry.begin)), 1)) {
    return kRNan;
  }

  // The kernel's call, as the code makes it (its NaN check runs when it is
  // called, on the filled planes). Its writes mustn't reach dsp's fields (the
  // pointer table it refuses itself).
  const uint32_t state_addr = LdW(Host(base, uint32_t(fields.begin)));
  if (PrepareArgs(base, state_addr, r->table, r->table, r->channels, r->n, ctx.r1.u32 - 176u,
                  false, kernel) != kOk) {
    return kRKernel;
  }
  for (const Range& w : kernel->writes) {
    if (Overlap(w, fields) || Overlap(w, dry)) return kROverlaps;
  }

  for (int c = 0; c < r->channels; ++c) r->writes.push_back(planes[c]);
  r->writes.push_back(out);
  r->writes.insert(r->writes.end(), kernel->writes.begin(), kernel->writes.end());
  return kROk;
}

void RunRead(PPCContext& ctx, uint8_t* base, const ReadCall& r) {
  const uint32_t channels = uint32_t(r.channels), n = r.n;

  // The planes, from the interleaved input.
  for (uint32_t c = 0; c < channels; ++c) {
    uint8_t* const p = r.plane[c];
    const uint8_t* src = r.in_h + 4 * c;
    for (uint32_t i = 0; i < n; ++i) std::memcpy(p + 4 * i, src + 4 * (size_t(i) * channels), 4);
  }

  // The kernel, through its hook, with the registers the code gives it.
  ctx.r3.u64 = LdW(Host(base, r.dsp + kReadState));
  ctx.r4.u64 = r.r3 + kReadPlanes;
  ctx.r5.u64 = ctx.r4.u64;
  ctx.r6.u64 = r.r7;
  ctx.r7.u64 = r.r6;
  ctx.f1.f64 = double(float(double(int64_t(int32_t(LdW(Host(base, r.dsp + kReadRate)))))));
  ctx.r1.u32 = uint32_t(r.r1) - 176u;
  sub_823A65A8(ctx, base);
  ctx.r1.u64 = r.r1;

  // out = in * dry + plane.
  const float dry = LdF(Host(base, r.dsp + kReadDry));
  for (uint32_t c = 0; c < channels; ++c) {
    const uint8_t* const p = r.plane[c];
    for (uint32_t i = 0; i < n; ++i) {
      const size_t at = 4 * (size_t(i) * channels + c);
      StF(r.out_h + at, Fma(LdF(r.in_h + at), dry, LdF(p + 4 * i)));
    }
  }
  ctx.r3.u64 = 0;
}

std::mutex g_read_lock;  // the read callback's check (taken before g_lock, never inside it)
int g_read_checks_left = kChecksPerShape;
uint64_t g_read_spot = 0;

}  // namespace

REX_HOOK_RAW(sub_82392B50) {
  static thread_local ReadCall call;
  static thread_local Call kernel;
  static thread_local std::vector<uint8_t> before, guest, native;

  if (!REXCVAR_GET(cod3_native_fmod) || g_read_off.load(std::memory_order_relaxed) ||
      g_nan.load(std::memory_order_relaxed)) {
    const uint64_t t0 = NowNs();
    __imp__sub_82392B50(ctx, base);
    g_read_guest_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    g_read_guest_timed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const ReadWhy why = ReadPrepare(ctx, base, &call, &kernel);
  if (why != kROk) {
    g_read_why[why].fetch_add(1, std::memory_order_relaxed);
    g_read_guest.fetch_add(1, std::memory_order_relaxed);
    if (why == kRNan && !g_nan.exchange(true)) {
      REXLOG_WARN("cod3: a NaN reached FMOD's reverb unit ({} channels, {} samples); the game's "
                  "code runs it from now on (NaNs can't be matched bit for bit)",
                  call.channels, call.n);
    }
    __imp__sub_82392B50(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();  // as the game's code does before its float math

  std::unique_lock<std::mutex> lock(g_read_lock);
  if (g_read_checks_left > 0 || ++g_read_spot % kSpotEvery == 0) {
    CopyOut(base, call.writes, &before);
    const uint64_t t0 = NowNs();
    __imp__sub_82392B50(ctx, base);  // ctx ends as the game's code leaves it (r3 = 0)
    g_read_guest_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    g_read_guest_timed.fetch_add(1, std::memory_order_relaxed);
    CopyOut(base, call.writes, &guest);
    CopyIn(base, call.writes, before);
    RunRead(ctx, base, call);
    CopyOut(base, call.writes, &native);
    g_read_checked.fetch_add(1, std::memory_order_relaxed);
    if (native != guest) {
      CopyIn(base, call.writes, guest);  // the game keeps the game's result
      g_read_off.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("cod3: native FMOD reverb unit differs from the game's code ({} channels, {} "
                   "samples; first at {}); the game's code runs it from now on",
                   call.channels, call.n, FirstDifference(call.writes, guest, native));
      return;
    }
    if (g_read_checks_left > 0 && --g_read_checks_left == 0) {
      REXLOG_INFO("cod3: native FMOD reverb unit (its buffers in and out): {} calls the same as "
                  "the game's code; native from now on",
                  kChecksPerShape);
    }
    return;
  }
  lock.unlock();
  const uint64_t t0 = NowNs();
  RunRead(ctx, base, call);
  g_read_native_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  g_read_native.fetch_add(1, std::memory_order_relaxed);
}
