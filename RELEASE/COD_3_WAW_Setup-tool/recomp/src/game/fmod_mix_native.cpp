// cod3 - [native-fmod-mix] FMOD's channel mix kernels as native code.
//
// FMOD Ex's software mixer adds every playing sound into the mix buffer through
// one of four small kernels, called from its connection mix (sub_823B07C8,
// cod3mp sub_8237FE50) once that has stepped the output to a 16-byte boundary:
//
//   out[] += in[] * gain, float samples, frames = r5:
//     sub_823BD5D8 (cod3mp sub_8238CC60): mono in, stereo out; gains f1 / f2
//       (left / right, doubles);
//     sub_823BD6C0 (cod3mp sub_8238CD48): stereo in, stereo out; f1 / f2;
//     sub_823BD7C0 (cod3mp sub_8238CE48): mono in, 6 channels out; gains at
//       r6, r7, r8 (four floats each: the six channels of two frames);
//     sub_823BD998 (cod3mp sub_8238D020): 6 channels in and out; r6, r7, r8 the
//       same way.
//   in r3, out r4. Each one copies its gains to the stack as floats (stfs, the
//   flush off), then runs groups of 8 frames (4 for the 6-channel ones) in VMX
//   (lvx / stvx, vmaddfp: a multiply, then an add, the vector unit's denormal
//   flush on), then the rest one frame at a time with fmadds (a double fma
//   rounded to float, the flush off), reading the gains from f1 / f2 or from
//   r6 / r7 (the first frame's six) again for every frame.
//
// The native version does the same operations in the same order for every
// output: the vector steps with the same SIMD calls as the generated code
// (simde mul, then add), each lane paired with the same gain lane, the scalar
// steps as the same std::fma on the same doubles (the fused operands in the
// same order), the flush switched the same way and left as the game's code
// leaves it (on only when groups ran and no single frames did).
//
// It runs only where that is provably what the game's code does, decided
// before anything is written: at most 65536 frames; in, out and the gains 4-byte
// aligned plain guest memory; when there are groups, in, out and the stack
// pointer 16-byte aligned (lvx and stvx drop the low four address bits, and the
// gain copies are read back from the stack with lvx); out over none of in, the
// gains and the stack below r1 the code writes, the gains and in over none of
// that stack either; no NaN or infinity in what it reads (which NaN comes out
// of two NaN operands depends on the instruction the compiler picked; see
// fmod_native.cpp), and gains f1 / f2 no larger than a float holds. Everything
// else goes to the game's code. The native version doesn't write the gain
// copies and saved registers the game's code leaves below the stack pointer
// (nothing reads them).
//
// At the first call a self-check runs the game's code and the native version
// on scratch guest memory for every kernel (frame counts around the group
// sizes and random ones, random samples and gains, denormals, zeros and large
// values among them) and compares the output, the bytes around it and the flush
// state; a kernel that differs stays on the game's code. In play the first 16
// calls of each kernel are run both ways too (the game's result kept), then one
// call in 2048. cod3_native_fmod_mix (on) switches it off at any time;
// cod3_native_fmod_mix_report writes the counts to the log.
//
// [2.12.14] While a sound's volume or pan changes, the connection mix uses its
// ramped matrix mix instead, sub_823AF9E0 (cod3mp sub_8237F068), with the ramp
// set up by sub_823AFB68 (cod3mp sub_8237F1F0) when the levels are set:
//
//   ramp_mix(conn r3, out r4, in r5, nout r6, nin r7, frames r8) -> 0:
//     per frame, per output channel o (out interleaved, nout a frame):
//       sum = 0.0 (a float constant); for each input i:
//         sum = fmadds(in[i], G_o[i], sum); G_o[i] = fadds(D_o[i], G_o[i])
//       out[o] = fadds(out[o], sum)
//     then conn+160 (the ramp's frames left) -= frames. G_o / D_o are the
//     connection's level and step arrays (pointers at conn+52 / conn+76).
//   ramp_setup(conn r3) -> 0: for each output o of conn+20, every level's
//     step D_o[i] = fmuls(fmsubs(T_o[i], conn+164, G_o[i]), a constant) (T_o
//     the target levels at conn+28; six of them when conn+24 is 6, else
//     conn+20 of them), and the |D| summed with fadds; when the sum is above
//     another constant, conn+160 = 64.
//   Both scalar, the flush off from their first float load.
//
// The native versions do the same operations in the same order (the 6-level
// setup's sum in the order its unrolled loop adds), on the same values. They
// run only where nothing they write (the output, the levels, the steps, the
// count) lies over anything else they read or write, everything is plain
// guest memory, nout at most 6 (the connection's slots), and no NaN or
// infinity is among what they read. The self-check and the live checks are the
// kernels' (200 cases each at the first call; then the first 16 calls and one
// in 2048 compared).
//
// [2.12.15] The connection mix itself, sub_823B07C8 (cod3mp sub_8237FE50):
//
//   conn_mix(conn r3, out r4, in r5, nout r6, nin r7, frames r8) -> 0:
//     a ramp in progress (conn+160) is mixed first with the ramped mix; then,
//     unless the volume (conn+164) is 0, the levels are loaded ([0] and [1] of
//     each of the six level arrays, [k] of the k-th; 0.0 for a null slot) and:
//       2 out, 1 in: frames one at a time (fmadds) to a 16-byte output, then
//         the mono-into-stereo kernel with levels [0][0], [1][0];
//       2 out, 2 in: if [0][1] or [1][0] reaches a threshold, out = in L x
//         level + in R x level per frame (two fmadds each); else one at a time
//         to a boundary, then the stereo kernel with [0][0], [1][1];
//       6 out, 1 in: one at a time to a boundary, then the mono-into-6 kernel;
//       6 out, 2 in: if any of the ten left / right levels reaches the
//         threshold, the full 2x6 sum per frame; else left into 0 and right
//         into 1 only;
//       6 out, 6 in at volume 1 with identity targets (sub_823AFD30): one at
//         a time to a boundary, then the 6-into-6 kernel with the diagonal;
//       anything else: each output += the sum (from 0.0) of in x level.
//   The native version takes every one of those paths with the same
//   operations in the same order (the kernels through their native code), and
//   leaves a ramp in progress to the game's code (whose ramp and kernels are
//   native anyway). It runs only where it is provably the same: at most 6
//   outputs and 32 inputs, plain aligned memory, the stack pointer 16-byte
//   aligned (the kernels' gain copies), the kernels' alignment where they
//   run, the output over nothing it reads and the stack frame over nothing at
//   all, no NaN or infinity read.
//
// Both programs: tools/mp_aliases.py points each function at its cod3mp twin
// (mp/config/campaign_map.tsv); the kernels read no constants, the ramp's and
// the connection mix's come through COD3_ADDR.

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
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

// No contraction of a * b + c into an fma anywhere here: the separate roundings
// are part of what the game's code computes.
#if defined(__clang__)
#pragma clang fp contract(off)
#pragma STDC FP_CONTRACT OFF
#endif

REXCVAR_DEFINE_BOOL(cod3_native_fmod_mix, true, "cod3/Performance",
                    "FMOD's mix kernels (every playing sound added into the mix with its volume "
                    "and pan) run as native code where that does exactly what the game's own code "
                    "does (checked at start and on their first calls; off: the translated Xbox 360 "
                    "code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_823BD5D8);  // mono into stereo (cod3mp sub_8238CC60)
REX_EXTERN(__imp__sub_823BD6C0);  // stereo into stereo (cod3mp sub_8238CD48)
REX_EXTERN(__imp__sub_823BD7C0);  // mono into 6 channels (cod3mp sub_8238CE48)
REX_EXTERN(__imp__sub_823BD998);  // 6 channels into 6 (cod3mp sub_8238D020)
REX_EXTERN(__imp__sub_823AF9E0);  // the ramped matrix mix (cod3mp sub_8237F068)
REX_EXTERN(__imp__sub_823AFB68);  // its ramp setup (cod3mp sub_8237F1F0)
REX_EXTERN(__imp__sub_823B07C8);  // the connection mix (cod3mp sub_8237FE50)

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
  return a.begin < b.end && b.begin < a.end && a.end > a.begin && b.end > b.begin;
}

bool Plain(uint8_t* base, const Range& r) {
  if (r.end <= r.begin) return true;  // empty
  if (r.begin < 0x10000) return false;  // a null pointer and its first page: the game's code
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
inline float BitsF(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline uint32_t FBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// A value the compiler must take as changed here, so nothing computed from it
// moves above this point (the flush mode switch before it).
template <class T>
inline T Here(T v) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : "+m"(v));
#endif
  return v;
}

// ---------------------------------------------------------------------------
// The kernels
// ---------------------------------------------------------------------------

enum Kernel : int {
  kMonoStereo = 0, kStereoStereo, kMonoSix, kSixSix,  // the vector kernels
  kRamp, kRampSetup,                                  // the ramped matrix mix and its setup
  kConn,                                              // the connection mix that calls them
  kKernelCount
};
constexpr const char* kKernelName[kKernelCount] = {"mono into stereo", "stereo into stereo",
                                                   "mono into 6 channels", "6 channels into 6",
                                                   "ramped mix", "ramp setup", "connection mix"};
constexpr uint32_t kInChannels[kKernelCount] = {1, 2, 1, 6};
constexpr uint32_t kOutChannels[kKernelCount] = {2, 2, 6, 6};
constexpr uint32_t kGroupShift[kKernelCount] = {3, 3, 2, 2};  // frames a VMX group: 8, 8, 4, 4
// The stack below r1 the game's code writes (gain copies, saved registers).
constexpr uint32_t kStackBelow[kKernelCount] = {16, 32, 64, 256};

constexpr uint32_t kMaxFrames = 65536;

inline bool PointerGains(Kernel k) { return k == kMonoSix || k == kSixSix; }

// One call, checked and laid out.
struct Call {
  Kernel kernel = kMonoStereo;
  uint32_t in = 0, out = 0, frames = 0, groups = 0, tail = 0;
  double f1 = 0, f2 = 0;      // the stereo kernels' gains (as the registers hold them)
  uint32_t gain_bits[12]{};   // the 6-channel kernels': r6[0..3], r7[0..3], r8[0..3]
  uint32_t out_bytes = 0, in_bytes = 0;
  bool flush_end = false;     // the flush left on (the last float step was a VMX one)
};

enum Why : int {
  kOk = 0,
  kShapeOut,  // more frames (or channels) than the native code takes
  kAlign,     // in, out, the gains or the stack not aligned as the game's code needs
  kMemory,    // an address that isn't plain guest memory
  kOverlaps,  // what the call writes over what it reads, or over the stack it writes
  kSpecial,   // a NaN or an infinity in what it reads, or a gain larger than a float
  kRamping,   // the connection mix with a ramp in progress (its ramp and kernels are native)
  kWhyCount
};
constexpr const char* kWhyName[kWhyCount] = {"ok",      "frames or channels",  "alignment",
                                             "memory",  "overlap", "nan or infinity",
                                             "ramping"};

// Any NaN or infinity among n big-endian floats.
bool AnySpecial(const uint8_t* p, uint32_t n) {
  uint32_t hit = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t w;
    std::memcpy(&w, p + 4 * i, 4);
    // the exponent bits of a big-endian float, read as a little-endian word
    hit |= ((w & 0x0000807Fu) == 0x0000807Fu) ? 1u : 0u;
  }
  return hit != 0;
}

inline bool FloatSized(double d) { return std::fabs(d) <= double(FLT_MAX); }  // false for NaN too

// The call's arguments as the code reads them, and whether the native code
// takes it (everything checked before anything is written).
Why Prepare(uint8_t* base, const PPCContext& ctx, Kernel k, Call* c) {
  c->kernel = k;
  c->in = ctx.r3.u32;
  c->out = ctx.r4.u32;
  c->frames = ctx.r5.u32;
  if (c->frames > kMaxFrames) return kShapeOut;
  c->groups = c->frames >> kGroupShift[k];
  c->tail = c->frames & ((1u << kGroupShift[k]) - 1u);
  c->in_bytes = 4u * c->frames * kInChannels[k];
  c->out_bytes = 4u * c->frames * kOutChannels[k];
  c->flush_end = c->groups > 0 && c->tail == 0;
  const uint32_t r1 = ctx.r1.u32;

  if ((c->in & 3) != 0 || (c->out & 3) != 0) return kAlign;
  if (c->groups > 0 && ((c->in & 15) != 0 || (c->out & 15) != 0 || (r1 & 15) != 0)) return kAlign;

  if (r1 < kStackBelow[k]) return kMemory;
  const Range in_r{c->in, uint64_t(c->in) + c->in_bytes};
  const Range out_r{c->out, uint64_t(c->out) + c->out_bytes};
  const Range stack_r{uint64_t(r1) - kStackBelow[k], r1};
  if (!Plain(base, in_r) || !Plain(base, out_r)) return kMemory;
  if (Overlap(out_r, in_r) || Overlap(out_r, stack_r) || Overlap(in_r, stack_r)) return kOverlaps;

  if (PointerGains(k)) {
    const uint32_t gp[3] = {ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
    for (int j = 0; j < 3; ++j) {
      if ((gp[j] & 3) != 0) return kAlign;
      const Range g{gp[j], uint64_t(gp[j]) + 16};
      if (!Plain(base, g)) return kMemory;
      if (Overlap(g, out_r) || Overlap(g, stack_r)) return kOverlaps;
      for (int i = 0; i < 4; ++i) c->gain_bits[4 * j + i] = LdW(Host(base, gp[j] + 4u * i));
    }
    // the gains the call reads: all twelve for groups, the first six for single frames
    const int used = c->groups > 0 ? 12 : c->tail > 0 ? 6 : 0;
    for (int i = 0; i < used; ++i) {
      if ((c->gain_bits[i] & 0x7F800000u) == 0x7F800000u) return kSpecial;
    }
  } else {
    c->f1 = ctx.f1.f64;
    c->f2 = ctx.f2.f64;
    if (!FloatSized(c->f1) || !FloatSized(c->f2)) return kSpecial;
  }
  if (AnySpecial(Host(base, c->in), c->in_bytes / 4) || AnySpecial(Host(base, c->out), c->out_bytes / 4))
    return kSpecial;
  return kOk;
}

// ---------------------------------------------------------------------------
// The arithmetic, step by step as the generated code does it
// ---------------------------------------------------------------------------

// Four big-endian floats into lanes 0..3 in memory order, and back. (The
// generated code keeps the lanes the other way round; each operation here is
// lane by lane, so only which lanes meet matters.)
inline simde__m128i SwapMask() {
  return simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
}
inline simde__m128 Ld4(const uint8_t* p, simde__m128i swap) {
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p)), swap));
}
inline void St4(uint8_t* p, simde__m128 v, simde__m128i swap) {
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(p),
                        simde_mm_shuffle_epi8(simde_mm_castps_si128(v), swap));
}
inline simde__m128 Bits4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return simde_mm_castsi128_ps(simde_mm_setr_epi32(int(a), int(b), int(c), int(d)));
}
// vmaddfp: a * g, then + o (two roundings), as the generated code writes it.
inline simde__m128 Madd(simde__m128 a, simde__m128 g, simde__m128 o) {
  return simde_mm_add_ps(simde_mm_mul_ps(a, g), o);
}
// Lane moves: [x0 x0 x1 x1], [x2 x2 x3 x3], [xk xk xk xk] (vmrghw, vmrglw, vpermwi).
inline simde__m128 DupLo(simde__m128 x) { return simde_mm_unpacklo_ps(x, x); }
inline simde__m128 DupHi(simde__m128 x) { return simde_mm_unpackhi_ps(x, x); }
template <int K>
inline simde__m128 Splat(simde__m128 x) {
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(x), K * 0x55));
}

// fmadds as the generated code computes it: a double fma rounded to float.
inline uint32_t Fmadds(double a, double b, double c) { return FBits(float(std::fma(a, b, c))); }
inline double Lf(const uint8_t* p) { return double(BitsF(LdW(p))); }  // lfs

// The whole call: in and the old output read from in / old (host pointers to
// the guest's big-endian floats), the new output written to dst (old itself for
// the call proper, a copy for a check). The flush switched as the game's code
// switches it (fpscr: the context's, or the check's copy).
void Compute(rex::ppc::FPSCRRegister& fpscr, const Call& c, const uint8_t* in, const uint8_t* old,
             uint8_t* dst) {
  const simde__m128i swap = SwapMask();
  fpscr.disableFlushMode();  // the gain copies (stfs / lfs)
  const Kernel k = c.kernel;
  uint32_t i = 0;  // frames done
  if (k == kMonoStereo || k == kStereoStereo) {
    const double f1 = Here(c.f1), f2 = Here(c.f2);
    const uint32_t g1 = FBits(float(f1)), g2 = FBits(float(f2));  // stfs f1 / f2
    if (c.groups > 0) {
      const simde__m128 g = Bits4(g1, g2, g1, g2);
      fpscr.enableFlushMode();
      for (uint32_t n = 0; n < c.groups; ++n, i += 8) {
        const uint8_t* s = in + 4u * kInChannels[k] * i;
        const uint8_t* o = old + 8u * i;
        uint8_t* d = dst + 8u * i;
        if (k == kMonoStereo) {
          const simde__m128 m0 = Ld4(s, swap), m1 = Ld4(s + 16, swap);
          const simde__m128 r0 = Madd(DupLo(m0), g, Ld4(o, swap));
          const simde__m128 r1 = Madd(DupHi(m0), g, Ld4(o + 16, swap));
          const simde__m128 r2 = Madd(DupLo(m1), g, Ld4(o + 32, swap));
          const simde__m128 r3 = Madd(DupHi(m1), g, Ld4(o + 48, swap));
          St4(d, r0, swap);
          St4(d + 16, r1, swap);
          St4(d + 32, r2, swap);
          St4(d + 48, r3, swap);
        } else {
          for (int v = 0; v < 4; ++v)
            St4(d + 16 * v, Madd(Ld4(s + 16 * v, swap), g, Ld4(o + 16 * v, swap)), swap);
        }
      }
    }
    if (c.tail > 0) {
      fpscr.disableFlushMode();
      const double h1 = Here(f1), h2 = Here(f2);
      for (; i < c.frames; ++i) {
        const uint8_t* s = in + 4u * kInChannels[k] * i;
        const uint8_t* o = old + 8u * i;
        uint8_t* d = dst + 8u * i;
        const double sl = Lf(s), sr = k == kMonoStereo ? sl : Lf(s + 4);
        StW(d, Fmadds(sl, h1, Lf(o)));      // fmadds in, f1, out
        StW(d + 4, Fmadds(sr, h2, Lf(o + 4)));
      }
    }
  } else {
    // the gain copies (lfs, then stfs: a float widened and narrowed again)
    uint32_t cp[12];
    for (int j = 0; j < 12; ++j) cp[j] = FBits(float(Here(double(BitsF(c.gain_bits[j])))));
    if (c.groups > 0) {
      const simde__m128 ga = Bits4(cp[0], cp[1], cp[2], cp[3]);    // r6's four
      const simde__m128 gb = Bits4(cp[4], cp[5], cp[6], cp[7]);    // r7's
      const simde__m128 gc = Bits4(cp[8], cp[9], cp[10], cp[11]);  // r8's
      fpscr.enableFlushMode();
      for (uint32_t n = 0; n < c.groups; ++n, i += 4) {
        const uint8_t* o = old + 24u * i;
        uint8_t* d = dst + 24u * i;
        if (k == kMonoSix) {
          const simde__m128 m = Ld4(in + 4u * i, swap);
          const simde__m128 r0 = Madd(Splat<0>(m), ga, Ld4(o, swap));
          const simde__m128 r1 = Madd(DupLo(m), gb, Ld4(o + 16, swap));
          const simde__m128 r2 = Madd(Splat<1>(m), gc, Ld4(o + 32, swap));
          const simde__m128 r3 = Madd(Splat<2>(m), ga, Ld4(o + 48, swap));
          const simde__m128 r4 = Madd(DupHi(m), gb, Ld4(o + 64, swap));
          const simde__m128 r5 = Madd(Splat<3>(m), gc, Ld4(o + 80, swap));
          St4(d, r0, swap);
          St4(d + 16, r1, swap);
          St4(d + 32, r2, swap);
          St4(d + 48, r3, swap);
          St4(d + 64, r4, swap);
          St4(d + 80, r5, swap);
        } else {
          const uint8_t* s = in + 24u * i;
          const simde__m128 gs[3] = {ga, gb, gc};
          for (int v = 0; v < 6; ++v)
            St4(d + 16 * v, Madd(Ld4(s + 16 * v, swap), gs[v % 3], Ld4(o + 16 * v, swap)), swap);
        }
      }
    }
    if (c.tail > 0) {
      fpscr.disableFlushMode();
      double g[6];  // lfs from r6 / r7 every frame: the first frame's six, as they are
      for (int j = 0; j < 6; ++j) g[j] = Here(double(BitsF(c.gain_bits[j])));
      for (; i < c.frames; ++i) {
        const uint8_t* o = old + 24u * i;
        uint8_t* d = dst + 24u * i;
        if (k == kMonoSix) {
          const double m = Lf(in + 4u * i);
          for (int ch = 0; ch < 6; ++ch) StW(d + 4 * ch, Fmadds(m, g[ch], Lf(o + 4 * ch)));
        } else {
          const uint8_t* s = in + 24u * i;
          for (int ch = 0; ch < 6; ++ch) {
            const double x = Lf(s + 4 * ch), y = Lf(o + 4 * ch);
            // the third channel's fmadds has the gain first (fmadds f0,f13,f0,f11)
            StW(d + 4 * ch, ch == 2 ? Fmadds(g[ch], x, y) : Fmadds(x, g[ch], y));
          }
        }
      }
    }
  }
  if (c.flush_end) fpscr.enableFlushMode();
  else fpscr.disableFlushMode();
}

void Guest(Kernel k, PPCContext& ctx, uint8_t* base) {
  switch (k) {
    case kMonoStereo: __imp__sub_823BD5D8(ctx, base); break;
    case kStereoStereo: __imp__sub_823BD6C0(ctx, base); break;
    case kMonoSix: __imp__sub_823BD7C0(ctx, base); break;
    case kSixSix: __imp__sub_823BD998(ctx, base); break;
    case kRamp: __imp__sub_823AF9E0(ctx, base); break;
    case kRampSetup: __imp__sub_823AFB68(ctx, base); break;
    default: __imp__sub_823B07C8(ctx, base); break;
  }
}

// The first difference between the game's output and the native one.
bool SameBytes(const uint8_t* guest, const uint8_t* mine, uint32_t bytes, uint32_t out_channels,
               std::string* why) {
  if (std::memcmp(guest, mine, bytes) == 0) return true;
  for (uint32_t k = 0; k + 4 <= bytes; k += 4) {
    const uint32_t g = LdW(guest + k), m = LdW(mine + k);
    if (g != m) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "output %u (frame %u): the game's code 0x%08X, native 0x%08X",
                    k / 4, k / 4 / out_channels, g, m);
      *why = buf;
      return false;
    }
  }
  *why = "the output differs";
  return false;
}

constexpr uint32_t FlushMask() { return uint32_t(rex::ppc::FPSCRRegister::FlushMask); }

// ---------------------------------------------------------------------------
// The ramped matrix mix and its setup
// ---------------------------------------------------------------------------

constexpr uint32_t kZeroAddr = COD3_ADDR(0x820014D4, 0x82001574);       // 0.0f: where the sums start
constexpr uint32_t kRampScaleAddr = COD3_ADDR(0x8204D178, 0x820857AC);  // the step's scale
constexpr uint32_t kRampEpsAddr = COD3_ADDR(0x820164B0, 0x820161A0);    // the "levels moved" threshold

// The connection's fields (offsets from r3).
constexpr uint32_t kOutCount = 20;   // s32: outputs (the setup's loops)
constexpr uint32_t kInCount = 24;    // s32: the setup takes its 6-level path when this is 6
constexpr uint32_t kTargets = 28;    // float*[6]: the levels being ramped to
constexpr uint32_t kGains = 52;      // float*[6]: the levels now
constexpr uint32_t kSteps = 76;      // float*[6]: what each frame adds to them
constexpr uint32_t kRampLeft = 160;  // s32: frames of ramp left
constexpr uint32_t kVolume = 164;    // float
constexpr int32_t kSlots = 6;
constexpr int32_t kMaxRampIn = 32;

inline bool Special(uint32_t bits) { return (bits & 0x7F800000u) == 0x7F800000u; }
inline double Rs(double v) { return double(float(v)); }  // every single-precision result
inline double AbsBits(double v) {  // fabs as the generated code does it (the sign bit cleared)
  uint64_t u;
  std::memcpy(&u, &v, 8);
  u &= ~0x8000000000000000ull;
  std::memcpy(&v, &u, 8);
  return v;
}

struct RampCall {
  uint32_t self = 0, out = 0, in = 0, frames = 0, nout = 0, nin = 0;
  uint32_t gains[kSlots]{}, steps[kSlots]{};
  uint32_t zero_bits = 0;
  uint32_t left_end = 0;  // conn+160 as the call leaves it
};

// The ramped mix's arguments as its code reads them, and whether the native
// code takes the call (everything checked before anything is written; only
// memory the game's code reads is read).
Why PrepareRampArgs(uint8_t* base, uint32_t self, uint32_t out, uint32_t in, int32_t nout, int32_t nin,
                    uint32_t frames, RampCall* c) {
  c->self = self;
  c->out = out;
  c->in = in;
  c->frames = frames;
  if (c->frames > kMaxFrames || nout < 1 || nout > kSlots || nin < 1 || nin > kMaxRampIn) return kShapeOut;
  c->nout = uint32_t(nout);
  c->nin = uint32_t(nin);
  if ((c->self & 3) != 0 || (c->out & 3) != 0 || (c->in & 3) != 0) return kAlign;
  const Range left{uint64_t(c->self) + kRampLeft, uint64_t(c->self) + kRampLeft + 4};
  if (!Plain(base, left)) return kMemory;
  c->left_end = LdW(Host(base, c->self + kRampLeft)) - c->frames;
  if (c->frames == 0) return kOk;  // the game's code only writes the count back

  const Range slots{uint64_t(c->self) + kGains, uint64_t(c->self) + kSteps + 4u * kSlots};
  const Range zero{kZeroAddr, kZeroAddr + 4ull};
  const Range in_r{c->in, uint64_t(c->in) + 4ull * c->nin * c->frames};
  const Range out_r{c->out, uint64_t(c->out) + 4ull * c->nout * c->frames};
  if (!Plain(base, slots) || !Plain(base, zero) || !Plain(base, in_r) || !Plain(base, out_r))
    return kMemory;
  Range g[kSlots], d[kSlots];
  for (uint32_t o = 0; o < c->nout; ++o) {
    c->gains[o] = LdW(Host(base, c->self + kGains + 4u * o));
    c->steps[o] = LdW(Host(base, c->self + kSteps + 4u * o));
    if ((c->gains[o] & 3) != 0 || (c->steps[o] & 3) != 0) return kAlign;
    g[o] = Range{c->gains[o], uint64_t(c->gains[o]) + 4ull * c->nin};
    d[o] = Range{c->steps[o], uint64_t(c->steps[o]) + 4ull * c->nin};
    if (!Plain(base, g[o]) || !Plain(base, d[o])) return kMemory;
  }
  // nothing written (the output, the levels, the count) over anything else it reads or writes
  const Range reads[] = {in_r, slots, zero, left};
  for (const Range& r : reads) {
    if (Overlap(out_r, r)) return kOverlaps;
  }
  for (uint32_t o = 0; o < c->nout; ++o) {
    if (Overlap(out_r, g[o]) || Overlap(out_r, d[o])) return kOverlaps;
    for (const Range& r : reads) {
      if (Overlap(g[o], r)) return kOverlaps;
    }
    for (uint32_t p = 0; p < c->nout; ++p) {
      if (Overlap(g[o], d[p]) || (p != o && Overlap(g[o], g[p]))) return kOverlaps;
    }
  }
  c->zero_bits = LdW(Host(base, kZeroAddr));
  if (Special(c->zero_bits)) return kSpecial;
  if (AnySpecial(Host(base, c->in), c->nin * c->frames) || AnySpecial(Host(base, c->out), c->nout * c->frames))
    return kSpecial;
  for (uint32_t o = 0; o < c->nout; ++o) {
    if (AnySpecial(Host(base, c->gains[o]), c->nin) || AnySpecial(Host(base, c->steps[o]), c->nin))
      return kSpecial;
  }
  return kOk;
}

Why PrepareRamp(uint8_t* base, const PPCContext& ctx, RampCall* c) {
  return PrepareRampArgs(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.s32, ctx.r7.s32, ctx.r8.u32, c);
}

// The whole mix: in and the steps read, the output and the levels read and
// written in place (the guest's memory, or copies of it for a check).
void ComputeRamp(rex::ppc::FPSCRRegister& fpscr, const RampCall& c, const uint8_t* in, uint8_t* out,
                 uint8_t* const* gains, const uint8_t* const* steps) {
  if (c.frames == 0) return;  // no float step: the flush untouched
  fpscr.disableFlushMode();   // lfs of the 0.0
  const double zero = Here(double(BitsF(c.zero_bits)));
  for (uint32_t f = 0; f < c.frames; ++f) {
    const uint8_t* x = in + 4u * c.nin * f;
    for (uint32_t o = 0; o < c.nout; ++o) {
      uint8_t* op = out + 4u * (c.nout * f + o);
      const double prev = Lf(op);
      double sum = zero;
      uint8_t* g = gains[o];
      const uint8_t* d = steps[o];
      for (uint32_t i = 0; i < c.nin; ++i) {
        const double gi = Lf(g + 4 * i);
        StW(g + 4 * i, FBits(float(Lf(d + 4 * i) + gi)));       // fadds step, level
        sum = double(float(std::fma(Lf(x + 4 * i), gi, sum)));  // fmadds in, level, sum
      }
      StW(op, FBits(float(prev + sum)));  // fadds out, sum
    }
  }
}

struct SetupCall {
  uint32_t self = 0;
  int32_t nout = 0;
  bool six = false;
  uint32_t len = 0;  // levels per output
  uint32_t targets[kSlots]{}, gains[kSlots]{}, steps[kSlots]{};
  uint32_t zero_bits = 0, scale_bits = 0, eps_bits = 0, volume_bits = 0;
};

Why PrepareSetup(uint8_t* base, const PPCContext& ctx, SetupCall* c) {
  c->self = ctx.r3.u32;
  if ((c->self & 3) != 0) return kAlign;
  const Range counts{uint64_t(c->self) + kOutCount, uint64_t(c->self) + kTargets};
  const Range zero{kZeroAddr, kZeroAddr + 4ull};
  if (!Plain(base, counts) || !Plain(base, zero)) return kMemory;
  c->nout = int32_t(LdW(Host(base, c->self + kOutCount)));
  c->six = int32_t(LdW(Host(base, c->self + kInCount))) == 6;
  c->zero_bits = LdW(Host(base, kZeroAddr));
  if (c->nout <= 0) return kOk;  // the game's code returns at once
  if (c->nout > kSlots) return kShapeOut;
  c->len = c->six ? 6u : uint32_t(c->nout);
  const Range slots{uint64_t(c->self) + kTargets, uint64_t(c->self) + kSteps + 4u * kSlots};
  const Range tail{uint64_t(c->self) + kRampLeft, uint64_t(c->self) + kVolume + 4};
  const Range scale{kRampScaleAddr, kRampScaleAddr + 4ull}, eps{kRampEpsAddr, kRampEpsAddr + 4ull};
  for (const Range& r : {slots, tail, scale, eps}) {
    if (!Plain(base, r)) return kMemory;
  }
  Range t[kSlots], g[kSlots], d[kSlots];
  for (int32_t o = 0; o < c->nout; ++o) {
    c->targets[o] = LdW(Host(base, c->self + kTargets + 4u * o));
    c->gains[o] = LdW(Host(base, c->self + kGains + 4u * o));
    c->steps[o] = LdW(Host(base, c->self + kSteps + 4u * o));
    if ((c->targets[o] & 3) != 0 || (c->gains[o] & 3) != 0 || (c->steps[o] & 3) != 0) return kAlign;
    t[o] = Range{c->targets[o], uint64_t(c->targets[o]) + 4ull * c->len};
    g[o] = Range{c->gains[o], uint64_t(c->gains[o]) + 4ull * c->len};
    d[o] = Range{c->steps[o], uint64_t(c->steps[o]) + 4ull * c->len};
    if (!Plain(base, t[o]) || !Plain(base, g[o]) || !Plain(base, d[o])) return kMemory;
  }
  // the steps (and the count) written over nothing else the call reads or writes
  for (int32_t o = 0; o < c->nout; ++o) {
    for (const Range& r : {counts, slots, tail, zero, scale, eps}) {
      if (Overlap(d[o], r)) return kOverlaps;
    }
    for (int32_t p = 0; p < c->nout; ++p) {
      if (Overlap(d[o], t[p]) || Overlap(d[o], g[p]) || (p != o && Overlap(d[o], d[p]))) return kOverlaps;
    }
  }
  c->scale_bits = LdW(Host(base, kRampScaleAddr));
  c->eps_bits = LdW(Host(base, kRampEpsAddr));
  c->volume_bits = LdW(Host(base, c->self + kVolume));
  for (uint32_t b : {c->zero_bits, c->scale_bits, c->eps_bits, c->volume_bits}) {
    if (Special(b)) return kSpecial;
  }
  for (int32_t o = 0; o < c->nout; ++o) {
    if (AnySpecial(Host(base, c->targets[o]), c->len) || AnySpecial(Host(base, c->gains[o]), c->len))
      return kSpecial;
  }
  return kOk;
}

// The steps into steps[o] (the guest's memory or copies); true when the code
// would set the count to 64.
bool ComputeSetup(rex::ppc::FPSCRRegister& fpscr, const SetupCall& c, const uint8_t* const* targets,
                  const uint8_t* const* gains, uint8_t* const* steps) {
  fpscr.disableFlushMode();  // lfs of the 0.0, before anything else
  double total = Here(double(BitsF(c.zero_bits)));
  if (c.nout <= 0) return false;
  const double scale = double(BitsF(c.scale_bits)), vol = double(BitsF(c.volume_bits));
  for (int32_t o = 0; o < c.nout; ++o) {
    const uint8_t* t = targets[o];
    const uint8_t* g = gains[o];
    uint8_t* d = steps[o];
    if (c.six) {
      double dv[6];
      // the first fmsubs has the volume first (fmsubs f12,f12,f11,f10)
      dv[0] = Rs(Rs(std::fma(vol, Lf(t), -Lf(g))) * scale);
      for (int k = 1; k < 6; ++k) dv[k] = Rs(Rs(std::fma(Lf(t + 4 * k), vol, -Lf(g + 4 * k))) * scale);
      for (int k = 0; k < 6; ++k) StW(d + 4 * k, FBits(float(dv[k])));
      double s = Rs(AbsBits(dv[0]) + total);  // fadds |d0|, total
      for (int k = 1; k < 6; ++k) s = Rs(s + AbsBits(dv[k]));
      total = s;
    } else {
      for (uint32_t i = 0; i < c.len; ++i) {
        const double dv = Rs(Rs(std::fma(Lf(t + 4 * i), vol, -Lf(g + 4 * i))) * scale);
        StW(d + 4 * i, FBits(float(dv)));
        total = Rs(AbsBits(dv) + total);  // fadds |d|, total
      }
    }
  }
  // fcmpu, blelr: anything not above the threshold leaves the count
  return total > double(BitsF(c.eps_bits));
}

// ---------------------------------------------------------------------------
// The connection mix
// ---------------------------------------------------------------------------

constexpr uint32_t kOneAddr = COD3_ADDR(0x820014D0, 0x82001570);    // 1.0f (the identity test)
constexpr uint32_t kCrossAddr = COD3_ADDR(0x8207C880, 0x82016F40);  // the cross-level threshold
constexpr uint32_t kConnFrame = 240;                                // the connection mix's stack frame

enum ConnPath : int {
  kPathNone = 0,       // nothing to mix (no frames, or the volume is 0)
  kPathMonoStereo,     // 1 into 2: one frame at a time to a 16-byte boundary, then the kernel
  kPathStereoStereo,   // 2 into 2, no cross levels: the same, then the kernel
  kPathStereoFull,     // 2 into 2 with cross levels: the full 2x2 sum per frame
  kPathMonoSix,        // 1 into 6: one at a time to a boundary, then the kernel
  kPathStereoSix,      // 2 into 6, no cross levels: left into 0, right into 1 only
  kPathStereoSixFull,  // 2 into 6 with cross levels: the full 2x6 sum per frame
  kPathSixSix,         // 6 into 6 at volume 1 and identity targets: one at a time, then the kernel
  kPathGeneric,        // anything else: each output the sum of every input times its level
  kPathCount
};
constexpr const char* kPathName[kPathCount] = {"none",         "1 into 2",       "2 into 2",
                                               "2 into 2 full", "1 into 6",       "2 into 6",
                                               "2 into 6 full", "6 into 6",       "general"};

// The ranges a call reads (a fixed list: no allocation per call).
struct Ranges {
  Range r[64];
  int n = 0;
  bool full = false;
  void push_back(const Range& x) {
    if (n < 64) r[n++] = x;
    else full = true;
  }
  const Range* begin() const { return r; }
  const Range* end() const { return r + n; }
};

struct ConnCall {
  uint32_t self = 0, out = 0, in = 0, frames = 0;
  int32_t nout = 0, nin = 0;
  bool volume_read = false;   // the volume's lfs ran (the flush off)
  ConnPath path = kPathNone;
  uint32_t zero_bits = 0;
  uint32_t lv[kSlots][kSlots]{};  // the levels the code loads into registers (0.0 for a slot of 0)
  uint32_t gslot[kSlots]{};       // the level pointers (the general path reads through them)
  uint32_t out_bytes = 0;
  uint32_t pre = 0;               // frames done one at a time before the kernel
  Call kc;                        // the kernel's call
};

// The identity test (sub_823AFD30 with 6, 6, cod3mp sub_8237F3B8): volume 1.0 and
// every target 1.0 on the diagonal, 0.0 off it; it stops at the first that
// isn't. Reads only what that code reads; false (with *why) if memory isn't plain.
bool IdentityTargets(uint8_t* base, const ConnCall& c, Ranges* reads, bool* identity) {
  *identity = false;
  const Range one{kOneAddr, kOneAddr + 4ull};
  if (!Plain(base, one)) return false;
  reads->push_back(one);
  const uint32_t one_bits = LdW(Host(base, kOneAddr));
  if (Special(one_bits)) return false;
  if (double(BitsF(LdW(Host(base, c.self + kVolume)))) != double(BitsF(one_bits))) return true;
  const double zero = double(BitsF(c.zero_bits)), one_v = double(BitsF(one_bits));
  for (uint32_t o = 0; o < 6; ++o) {
    const uint32_t t = LdW(Host(base, c.self + kTargets + 4 * o));
    for (uint32_t i = 0; i < 6; ++i) {
      if ((t & 3) != 0) return false;
      const Range r{uint64_t(t) + 4 * i, uint64_t(t) + 4 * i + 4};
      if (!Plain(base, r)) return false;
      reads->push_back(r);
      const uint32_t x = LdW(Host(base, t + 4 * i));
      if (Special(x)) return false;
      if (double(BitsF(x)) != (o == i ? one_v : zero)) return true;
    }
  }
  *identity = true;
  return true;
}

// The connection mix's arguments as its code reads them, and whether the
// native code takes the call (everything checked before anything is written;
// only memory the game's code reads is read). A ramp in progress (conn+160 not
// 0) goes to the game's code, whose ramp and kernels are native themselves.
Why PrepareConn(uint8_t* base, const PPCContext& ctx, ConnCall* c) {
  c->self = ctx.r3.u32;
  c->out = ctx.r4.u32;
  c->in = ctx.r5.u32;
  c->nout = ctx.r6.s32;
  c->nin = ctx.r7.s32;
  c->frames = ctx.r8.u32;
  if (c->frames > kMaxFrames || c->nout > kSlots || c->nin > kMaxRampIn || c->nout < 0 || c->nin < 0)
    return kShapeOut;
  if ((c->self & 3) != 0 || (c->out & 3) != 0 || (c->in & 3) != 0) return kAlign;
  const uint32_t r1 = ctx.r1.u32;
  if ((r1 & 15) != 0) return kAlign;  // the kernels read their gain copies back with lvx
  if (r1 < kConnFrame + 256) return kMemory;
  const Range fields{uint64_t(c->self) + kOutCount, uint64_t(c->self) + kVolume + 4};
  if (!Plain(base, fields)) return kMemory;
  if (LdW(Host(base, c->self + kRampLeft)) != 0) return kRamping;
  Ranges reads;
  reads.push_back(fields);
  const uint32_t nout = uint32_t(c->nout), nin = uint32_t(c->nin);
  c->out_bytes = 4u * nout * c->frames;
  const Range in_r{c->in, uint64_t(c->in) + 4ull * nin * c->frames};
  const Range out_r{c->out, uint64_t(c->out) + c->out_bytes};
  const Range stack{uint64_t(r1) - kConnFrame - 256, r1};  // its frame and what the kernels write below it
  if (c->frames == 0) return kOk;  // the code returns before any float step
  const Range zero{kZeroAddr, kZeroAddr + 4ull};
  if (!Plain(base, zero) || !Plain(base, in_r) || !Plain(base, out_r)) return kMemory;
  reads.push_back(zero);
  reads.push_back(in_r);
  c->zero_bits = LdW(Host(base, kZeroAddr));
  const uint32_t vol_bits = LdW(Host(base, c->self + kVolume));
  if (Special(c->zero_bits) || Special(vol_bits)) return kSpecial;
  c->volume_read = true;
  if (double(BitsF(vol_bits)) == double(BitsF(c->zero_bits))) {
    c->path = kPathNone;  // fcmpu with 0.0, beq: returns (having read nothing more)
    return kOk;
  } else {
    // The levels the code loads into registers: [0] and [1] of each of the six,
    // [k] of the k-th for k = 2..5; a slot of 0 gives 0.0.
    for (uint32_t o = 0; o < uint32_t(kSlots); ++o) {
      const uint32_t g = LdW(Host(base, c->self + kGains + 4 * o));
      c->gslot[o] = g;
      const uint32_t need = o >= 2 ? o + 1 : 2;
      for (uint32_t i = 0; i < uint32_t(kSlots); ++i) c->lv[o][i] = c->zero_bits;
      if (g == 0) continue;
      if ((g & 3) != 0) return kAlign;
      const Range r{g, uint64_t(g) + 4ull * need};
      if (!Plain(base, r)) return kMemory;
      reads.push_back(r);
      for (uint32_t i : {0u, 1u, o >= 2 ? o : 1u}) {
        c->lv[o][i] = LdW(Host(base, g + 4 * i));
        if (Special(c->lv[o][i])) return kSpecial;
      }
    }
    auto cross_above = [&](std::initializer_list<std::pair<int, int>> cells, bool* above) {
      const Range thr{kCrossAddr, kCrossAddr + 4ull};
      if (!Plain(base, thr)) return false;
      reads.push_back(thr);
      const uint32_t t = LdW(Host(base, kCrossAddr));
      if (Special(t)) return false;
      *above = false;
      for (const auto& cell : cells) {
        if (double(BitsF(c->lv[cell.first][cell.second])) >= double(BitsF(t))) *above = true;  // fcmpu, bge
      }
      return true;
    };
    bool above = false;
    c->path = kPathGeneric;
    if (c->nout == 2 && c->nin == 1) {
      c->path = kPathMonoStereo;
    } else if (c->nout == 2 && c->nin == 2) {
      if (!cross_above({{0, 1}, {1, 0}}, &above)) return kSpecial;
      c->path = above ? kPathStereoFull : kPathStereoStereo;
    } else if (c->nout == 6 && c->nin == 1) {
      c->path = kPathMonoSix;
    } else if (c->nout == 6 && c->nin == 2) {
      if (!cross_above({{0, 1}, {1, 0}, {2, 0}, {2, 1}, {3, 0}, {3, 1}, {4, 0}, {4, 1}, {5, 0}, {5, 1}}, &above))
        return kSpecial;
      c->path = above ? kPathStereoSixFull : kPathStereoSix;
    } else if (c->nout == 6 && c->nin == 6) {
      bool identity = false;
      if (!IdentityTargets(base, *c, &reads, &identity)) return kMemory;
      if (identity) c->path = kPathSixSix;
    }
    if (c->path == kPathGeneric && nout > 0 && nin > 0) {
      for (uint32_t o = 0; o < nout; ++o) {
        const uint32_t g = c->gslot[o];
        if (g == 0 || (g & 3) != 0) return kMemory;  // the code would read through it anyway
        const Range r{g, uint64_t(g) + 4ull * nin};
        if (!Plain(base, r)) return kMemory;
        reads.push_back(r);
        if (AnySpecial(Host(base, g), nin)) return kSpecial;
      }
    }
    // The kernel paths: the frames done one at a time to a 16-byte boundary, then the kernel.
    Kernel k = kMonoStereo;
    uint32_t in_step = 0, out_step = 0;
    switch (c->path) {
      case kPathMonoStereo: k = kMonoStereo; in_step = 4; out_step = 8; break;
      case kPathStereoStereo: k = kStereoStereo; in_step = 8; out_step = 8; break;
      case kPathMonoSix: k = kMonoSix; in_step = 4; out_step = 24; break;
      case kPathSixSix: k = kSixSix; in_step = 24; out_step = 24; break;
      default: break;
    }
    if (out_step) {
      uint32_t p = c->out, q = c->in, f = c->frames, pre = 0;
      while ((p & 15) != 0 && f != 0) {
        p += out_step;
        q += in_step;
        --f;
        ++pre;
      }
      c->pre = pre;
      Call& kc = c->kc;
      kc.kernel = k;
      kc.in = q;
      kc.out = p;
      kc.frames = f;
      kc.groups = f >> kGroupShift[k];
      kc.tail = f & ((1u << kGroupShift[k]) - 1u);
      kc.in_bytes = 4u * f * kInChannels[k];
      kc.out_bytes = 4u * f * kOutChannels[k];
      kc.flush_end = kc.groups > 0 && kc.tail == 0;
      if (kc.groups > 0 && ((q & 15) != 0 || (p & 15) != 0)) return kAlign;  // lvx / stvx
      const auto& L = c->lv;
      if (k == kMonoStereo) {
        kc.f1 = double(BitsF(L[0][0]));
        kc.f2 = double(BitsF(L[1][0]));
      } else if (k == kStereoStereo) {
        kc.f1 = double(BitsF(L[0][0]));
        kc.f2 = double(BitsF(L[1][1]));  // fmr f2,f11
      } else if (k == kMonoSix) {
        const uint32_t g[12] = {L[0][0], L[1][0], L[2][0], L[3][0], L[4][0], L[5][0],
                                L[0][0], L[1][0], L[2][0], L[3][0], L[4][0], L[5][0]};
        std::memcpy(kc.gain_bits, g, sizeof(g));
      } else {
        const uint32_t g[12] = {L[0][0], L[1][1], L[2][2], L[3][3], L[4][4], L[5][5],
                                L[0][0], L[1][1], L[2][2], L[3][3], L[4][4], L[5][5]};
        std::memcpy(kc.gain_bits, g, sizeof(g));
      }
    }
  }
  // what it writes (the output; the stack below r1) over nothing it reads
  if (reads.full) return kMemory;
  for (const Range& r : reads) {
    if (Overlap(out_r, r) || Overlap(stack, r)) return kOverlaps;
  }
  if (Overlap(out_r, stack)) return kOverlaps;
  // (the general path with no outputs reads no input)
  const bool reads_in = !(c->path == kPathGeneric && nout == 0);
  if ((reads_in && AnySpecial(Host(base, c->in), nin * c->frames)) ||
      AnySpecial(Host(base, c->out), nout * c->frames))
    return kSpecial;
  return kOk;
}

// The paths: in read, the output read and written at out (the guest's memory
// or a copy); levels[o]: where the general path reads level o. (The flush
// around them is ComputeConn's.)
void ComputeConnBody(rex::ppc::FPSCRRegister& fpscr, const ConnCall& c, const uint8_t* in, uint8_t* out,
                     const uint8_t* const* levels) {
  if (!c.volume_read) return;  // no frames: nothing after the entry
  fpscr.disableFlushMode();    // lfs of the volume
  if (c.path == kPathNone) return;
  auto L = [&](int o, int i) { return double(BitsF(c.lv[o][i])); };
  auto F = [](uint32_t bits) { return double(BitsF(bits)); };
  const uint32_t n = c.frames;
  switch (c.path) {
    case kPathMonoStereo: {
      const double g0 = L(0, 0), g1 = L(1, 0);
      for (uint32_t f = 0; f < c.pre; ++f) {
        const double m = Lf(in + 4 * f);
        uint8_t* o = out + 8 * f;
        StW(o, Fmadds(m, g0, Lf(o)));
        StW(o + 4, Fmadds(m, g1, Lf(o + 4)));
      }
      Compute(fpscr, c.kc, in + 4 * c.pre, out + 8 * c.pre, out + 8 * c.pre);
      break;
    }
    case kPathStereoStereo: {
      const double g0 = L(0, 0), g1 = L(1, 1);
      for (uint32_t f = 0; f < c.pre; ++f) {
        const uint8_t* x = in + 8 * f;
        uint8_t* o = out + 8 * f;
        StW(o, Fmadds(Lf(x), g0, Lf(o)));
        StW(o + 4, Fmadds(Lf(x + 4), g1, Lf(o + 4)));
      }
      Compute(fpscr, c.kc, in + 8 * c.pre, out + 8 * c.pre, out + 8 * c.pre);
      break;
    }
    case kPathStereoFull: {
      for (uint32_t f = 0; f < n; ++f) {
        const double l = Lf(in + 8 * f), r = Lf(in + 8 * f + 4);
        uint8_t* o = out + 8 * f;
        for (int ch = 0; ch < 2; ++ch) {
          const uint32_t a = Fmadds(l, L(ch, 0), Lf(o + 4 * ch));
          StW(o + 4 * ch, Fmadds(r, L(ch, 1), F(a)));
        }
      }
      break;
    }
    case kPathMonoSix: {
      for (uint32_t f = 0; f < c.pre; ++f) {
        const double m = Lf(in + 4 * f);
        uint8_t* o = out + 24 * f;
        for (int ch = 0; ch < 6; ++ch) StW(o + 4 * ch, Fmadds(m, L(ch, 0), Lf(o + 4 * ch)));
      }
      Compute(fpscr, c.kc, in + 4 * c.pre, out + 24 * c.pre, out + 24 * c.pre);
      break;
    }
    case kPathStereoSix: {
      const double g0 = L(0, 0), g1 = L(1, 1);
      for (uint32_t f = 0; f < n; ++f) {
        const uint8_t* x = in + 8 * f;
        uint8_t* o = out + 24 * f;
        StW(o, Fmadds(Lf(x), g0, Lf(o)));
        StW(o + 4, Fmadds(Lf(x + 4), g1, Lf(o + 4)));
      }
      break;
    }
    case kPathStereoSixFull: {
      for (uint32_t f = 0; f < n; ++f) {
        const double l = Lf(in + 8 * f), r = Lf(in + 8 * f + 4);
        uint8_t* o = out + 24 * f;
        for (int ch = 0; ch < 6; ++ch) {
          const uint32_t a = Fmadds(l, L(ch, 0), Lf(o + 4 * ch));
          StW(o + 4 * ch, Fmadds(r, L(ch, 1), F(a)));
        }
      }
      break;
    }
    case kPathSixSix: {
      for (uint32_t f = 0; f < c.pre; ++f) {
        const uint8_t* x = in + 24 * f;
        uint8_t* o = out + 24 * f;
        for (int ch = 0; ch < 6; ++ch) StW(o + 4 * ch, Fmadds(Lf(x + 4 * ch), L(ch, ch), Lf(o + 4 * ch)));
      }
      Compute(fpscr, c.kc, in + 24 * c.pre, out + 24 * c.pre, out + 24 * c.pre);
      break;
    }
    default: {  // general
      const uint32_t nout = uint32_t(c.nout), nin = uint32_t(c.nin);
      const double zero = F(c.zero_bits);
      for (uint32_t f = 0; f < n; ++f) {
        const uint8_t* x = in + 4 * nin * f;
        for (uint32_t o = 0; o < nout; ++o) {
          uint8_t* op = out + 4 * (nout * f + o);
          const double prev = Lf(op);
          double sum = zero;
          for (uint32_t i = 0; i < nin; ++i)
            sum = double(float(std::fma(Lf(x + 4 * i), Lf(levels[o] + 4 * i), sum)));  // fmadds
          StW(op, FBits(float(prev + sum)));  // fadds out, sum
        }
      }
      break;
    }
  }
}

// The whole call, with the flush switched as the code switches it.
void ComputeConn(rex::ppc::FPSCRRegister& fpscr, const ConnCall& c, const uint8_t* in, uint8_t* out,
                 const uint8_t* const* levels) {
  // The code saves f26-f31 (__savefpr_26: stfd) on entry and restores them
  // (__restfpr_26: lfd) on every way out: the flush is off at both ends.
  fpscr.disableFlushMode();
  ComputeConnBody(fpscr, c, in, out, levels);
  fpscr.disableFlushMode();
}

// ---------------------------------------------------------------------------
// The checks and the counters
// ---------------------------------------------------------------------------

constexpr int kChecksPerKernel = 16;
constexpr uint64_t kSpotEvery = 2048;  // after those, one call in this many is checked too

std::mutex g_lock;
std::atomic<int> g_state{0};  // 0 the self-check not run yet, 1 running, 2 done
std::atomic<bool> g_ok[kKernelCount]{};
int g_checks_left[kKernelCount];
uint64_t g_spot = 0;
std::atomic<uint64_t> g_native[kKernelCount]{}, g_checked[kKernelCount]{}, g_guest{0};
std::atomic<uint64_t> g_why[kWhyCount]{};
std::atomic<uint64_t> g_paths[kPathCount]{};  // the connection mix's native calls by path
std::atomic<uint64_t> g_native_ns{0}, g_native_frames{0};
std::chrono::steady_clock::time_point g_next_report{};

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void Report() {
  std::string per;
  for (int k = 0; k < kKernelCount; ++k) {
    const uint64_t n = g_native[k].load(std::memory_order_relaxed);
    const uint64_t c = g_checked[k].load(std::memory_order_relaxed);
    if (!n && !c) continue;
    char part[128];
    std::snprintf(part, sizeof(part), "%s%s %llu (%llu compared)%s", per.empty() ? "" : ", ",
                  kKernelName[k], static_cast<unsigned long long>(n),
                  static_cast<unsigned long long>(c), g_ok[k].load() ? "" : " OFF");
    per += part;
  }
  std::string why;
  for (int k = 1; k < kWhyCount; ++k) {
    const uint64_t v = g_why[k].load(std::memory_order_relaxed);
    if (v) why += std::string(why.empty() ? "" : ", ") + kWhyName[k] + " " + std::to_string(v);
  }
  std::string paths;
  for (int p = 1; p < kPathCount; ++p) {
    const uint64_t v = g_paths[p].load(std::memory_order_relaxed);
    if (v) paths += std::string(paths.empty() ? "" : ", ") + kPathName[p] + " " + std::to_string(v);
  }
  if (!paths.empty()) paths = "; connection mixes run natively by path: " + paths;
  const uint64_t frames = g_native_frames.load(std::memory_order_relaxed);
  REXLOG_INFO("cod3: native FMOD mix kernels so far: calls run natively: {}; {} by the game's "
              "code{}{}{}{}; the kernels {:.2f} ns a frame",
              per.empty() ? "none" : per, g_guest.load(), why.empty() ? "" : " (", why,
              why.empty() ? "" : ")", paths,
              frames ? double(g_native_ns.load()) / double(frames) : 0.0);
}

void MaybeReport() {
  const auto now = std::chrono::steady_clock::now();
  if (now < g_next_report) return;
  const bool first = g_next_report == std::chrono::steady_clock::time_point{};
  g_next_report = now + (first ? std::chrono::seconds(60) : std::chrono::minutes(10));
  if (!first) Report();
}

void ReportCommand(std::string_view) { Report(); }

// ---------------------------------------------------------------------------
// The start-up self-check: both ways on scratch guest memory
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

// The flush mode both runs of a case start from: on or off at random (so a
// function that leaves it alone is told from one that switches it).
bool StartFlush(rex::ppc::FPSCRRegister& fp, Rng& rng) {
  const bool on = rng.Below(2) != 0;
  if (on) fp.enableFlushMode();
  else fp.disableFlushMode();
  return on;
}
void SetFlush(rex::ppc::FPSCRRegister& fp, bool on) {
  fp.csr = fp.getcsr();
  if (on) fp.enableFlushMode();
  else fp.disableFlushMode();
}

constexpr uint32_t kScratch = 256 * 1024;
constexpr uint32_t kInAt = 0;            // in, up to 64 KB
constexpr uint32_t kOutAt = 96 * 1024;   // the output, up to 64 KB (with the bytes around it)
constexpr uint32_t kGainAt = 192 * 1024; // r6 / r7 / r8's floats, 16 bytes apart
constexpr uint32_t kOutMargin = 64;      // bytes compared on each side of the output

// A float of every kind but NaN and infinity.
uint32_t AnyFloat(Rng& rng) {
  const uint32_t kind = rng.Below(10);
  const uint32_t sign = rng.Below(2) ? 0x80000000u : 0u;
  if (kind == 0) return sign;                                                // +-0
  if (kind == 1) return sign | (rng.Below(0x7FFFFF) + 1);                    // a denormal
  if (kind == 2) return sign | ((1u + rng.Below(0xFE)) << 23) | rng.Below(0x800000);  // any size
  if (kind == 3) return sign | (0x7F000000u + rng.Below(0x7FFFFF));          // near the largest
  const float v = float(int32_t(rng.Below(2000001)) - 1000000) / 1000000.0f;  // everyday [-1, 1]
  return FBits(v);
}

// A gain: mostly everyday volumes, sometimes any float.
uint32_t AnyGain(Rng& rng) {
  if (rng.Below(4) == 0) return AnyFloat(rng);
  return FBits(float(rng.Below(200001)) / 100000.0f);  // [0, 2]
}

// f1 / f2: a float widened, any double in [-4, 4], one below float's smallest
// normal, or a double denormal.
double AnyGainDouble(Rng& rng) {
  switch (rng.Below(6)) {
    case 0: return double(float(rng.Below(200001)) / 100000.0f);
    case 1: return (double(int64_t(rng.Next() >> 11)) / double(1ull << 53) - 0.5) * 8.0;
    case 2: return 3.0e-39 * double(1 + rng.Below(100));
    case 3: return 1.0e-310 * double(1 + rng.Below(100));
    case 4: return 0.0;
    default: {
      uint32_t b = AnyFloat(rng);
      return double(BitsF(b));
    }
  }
}

uint32_t AnyFrames(Rng& rng, Kernel k) {
  const uint32_t g = 1u << kGroupShift[k];
  switch (rng.Below(4)) {
    case 0: return rng.Below(2 * g + 1);                         // around one or two groups
    case 1: return g * (1 + rng.Below(8)) + rng.Below(2) * (g - 1);
    default: return rng.Below(1100);
  }
}

// One case on the scratch block: the native version on a copy, then the game's
// code. false (with *why) if they differ.
bool RunCase(PPCContext& parent, uint8_t* base, uint32_t scratch, Kernel k, Rng& rng, bool* ran,
             std::string* why) {
  uint8_t* host = Host(base, scratch);
  const uint32_t frames = AnyFrames(rng, k);
  const uint32_t in_bytes = 4u * frames * kInChannels[k], out_bytes = 4u * frames * kOutChannels[k];
  if (in_bytes > 64 * 1024 || out_bytes + 2 * kOutMargin + 64 > 64 * 1024) return true;
  // 16-byte aligned mostly (groups need it); 4-byte for calls without groups
  const bool odd = (frames >> kGroupShift[k]) == 0 && rng.Below(2) == 0;
  const uint32_t in = scratch + kInAt + 64 + (odd ? 4u * rng.Below(4) : 0u);
  const uint32_t out = scratch + kOutAt + kOutMargin + 64 + (odd ? 4u * rng.Below(4) : 0u);
  for (uint32_t b = 0; b < in_bytes; b += 4) StW(Host(base, in + b), AnyFloat(rng));
  uint8_t* oarea = Host(base, out - kOutMargin);
  for (uint32_t b = 0; b < out_bytes + 2 * kOutMargin; ++b) oarea[b] = uint8_t(0xA5 ^ b);
  for (uint32_t b = 0; b < out_bytes; b += 4) StW(Host(base, out + b), AnyFloat(rng));
  for (uint32_t b = 0; b < 48; b += 4) StW(host + kGainAt + b, AnyGain(rng));

  rex::CallFrame frame(parent);
  PPCContext& g = frame.ctx;
  g.r1.u32 = (parent.r1.u32 - 0x1000) & ~0xFu;
  g.r3.u64 = in;
  g.r4.u64 = out;
  g.r5.u64 = frames;
  g.f1.f64 = AnyGainDouble(rng);
  g.f2.f64 = AnyGainDouble(rng);
  g.r6.u64 = scratch + kGainAt;
  g.r7.u64 = scratch + kGainAt + 16;
  g.r8.u64 = scratch + kGainAt + 32;

  Call call;
  if (Prepare(base, g, k, &call) != kOk) return true;  // not a native case
  // The native version first, into a copy, then the game's code, each from the
  // real control word as the other left it (the cached copies resynced: the
  // generated code only switches the flush mode when its copy says it differs).
  std::vector<uint8_t> mine(Host(base, out), Host(base, out) + out_bytes);
  rex::ppc::FPSCRRegister fp = parent.fpscr;
  fp.csr = fp.getcsr();
  const bool start_flush = StartFlush(fp, rng);
  Compute(fp, call, Host(base, in), mine.data(), mine.data());
  SetFlush(g.fpscr, start_flush);
  Guest(k, g, base);
  *ran = true;
  if (!SameBytes(Host(base, out), mine.data(), out_bytes, kOutChannels[k], why)) {
    *why += " (" + std::to_string(frames) + " frames)";
    return false;
  }
  for (uint32_t b = 0; b < kOutMargin; ++b) {
    if (oarea[b] != uint8_t(0xA5 ^ b) ||
        oarea[kOutMargin + out_bytes + b] != uint8_t(0xA5 ^ (kOutMargin + out_bytes + b))) {
      *why = "the game's code wrote outside the output (the check's own mistake)";
      return false;
    }
  }
  const bool guest_flush = (g.fpscr.csr & FlushMask()) == FlushMask();
  if (guest_flush != call.flush_end) {
    *why = std::string("the flush mode left: the game's code ") + (guest_flush ? "on" : "off") +
           ", native " + (call.flush_end ? "on" : "off");
    return false;
  }
  return true;
}


constexpr uint32_t kObjAt = 200 * 1024;     // a connection (its first 200 bytes)
constexpr uint32_t kArraysAt = 204 * 1024;  // its level, step and target arrays, 256 bytes apart
constexpr uint32_t kArrayStride = 256;
constexpr uint32_t kGainsAt = kArraysAt, kStepsAt = kArraysAt + 6 * 256, kTargetsAt = kArraysAt + 12 * 256;

// A step: mostly small, sometimes any float.
uint32_t AnyStep(Rng& rng) {
  if (rng.Below(4) == 0) return AnyFloat(rng);
  return FBits(float(int32_t(rng.Below(2001)) - 1000) / 64000.0f);
}

// A connection for the ramp's two functions, laid out on the scratch block.
void MakeConnection(uint8_t* base, uint32_t scratch, Rng& rng) {
  uint8_t* obj = Host(base, scratch + kObjAt);
  for (uint32_t b = 0; b < 200; ++b) obj[b] = uint8_t(0x5A ^ b);
  for (uint32_t o = 0; o < uint32_t(kSlots); ++o) {
    StW(obj + kTargets + 4 * o, scratch + kTargetsAt + kArrayStride * o);
    StW(obj + kGains + 4 * o, scratch + kGainsAt + kArrayStride * o);
    StW(obj + kSteps + 4 * o, scratch + kStepsAt + kArrayStride * o);
  }
  for (uint32_t b = 0; b < 6 * kArrayStride; b += 4) {
    StW(Host(base, scratch + kGainsAt + b), AnyGain(rng));
    StW(Host(base, scratch + kStepsAt + b), AnyStep(rng));
    StW(Host(base, scratch + kTargetsAt + b), AnyGain(rng));
  }
  StW(obj + kRampLeft, uint32_t(rng.Next()));
  StW(obj + kVolume, AnyGain(rng));
}

bool CompareArea(const uint8_t* guest, const uint8_t* mine, uint32_t bytes, const char* what, std::string* why) {
  if (std::memcmp(guest, mine, bytes) == 0) return true;
  for (uint32_t k = 0; k + 4 <= bytes; k += 4) {
    const uint32_t g = LdW(guest + k), m = LdW(mine + k);
    if (g != m) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s word %u: the game's code 0x%08X, native 0x%08X", what, k / 4, g, m);
      *why = buf;
      return false;
    }
  }
  *why = std::string(what) + " differs";
  return false;
}

// One ramped mix case: native on copies, then the game's code.
bool RunRampCase(PPCContext& parent, uint8_t* base, uint32_t scratch, Rng& rng, bool* ran, std::string* why) {
  const uint32_t pick[] = {1, 2, 3, 4, 5, 6, 6, 6, 7, 8, 13, 32};
  const uint32_t nout = 1 + rng.Below(6), nin = pick[rng.Below(12)];
  uint32_t frames = rng.Below(4) == 0 ? rng.Below(4) : rng.Below(300);
  frames = std::min(frames, (32u * 1024u) / (4u * std::max(nin, nout)));
  MakeConnection(base, scratch, rng);
  const uint32_t self = scratch + kObjAt, in = scratch + kInAt + 64;
  const uint32_t out = scratch + kOutAt + kOutMargin + 64;
  const uint32_t out_bytes = 4u * nout * frames;
  for (uint32_t b = 0; b < 4u * nin * frames; b += 4) StW(Host(base, in + b), AnyFloat(rng));
  uint8_t* oarea = Host(base, out - kOutMargin);
  for (uint32_t b = 0; b < out_bytes + 2 * kOutMargin; ++b) oarea[b] = uint8_t(0xA5 ^ b);
  for (uint32_t b = 0; b < out_bytes; b += 4) StW(Host(base, out + b), AnyFloat(rng));

  rex::CallFrame frame(parent);
  PPCContext& g = frame.ctx;
  g.r1.u32 = (parent.r1.u32 - 0x1000) & ~0xFu;
  g.r3.u64 = self;
  g.r4.u64 = out;
  g.r5.u64 = in;
  g.r6.s64 = int32_t(nout);
  g.r7.s64 = int32_t(nin);
  g.r8.u64 = frames;
  RampCall call;
  if (PrepareRamp(base, g, &call) != kOk) return true;
  // native first, on copies of everything it writes
  std::vector<uint8_t> area(Host(base, out - kOutMargin), Host(base, out - kOutMargin) + out_bytes + 2 * kOutMargin);
  std::vector<uint8_t> levels(Host(base, scratch + kGainsAt), Host(base, scratch + kGainsAt) + 6 * kArrayStride);
  uint8_t* gains[kSlots];
  const uint8_t* steps[kSlots];
  for (uint32_t o = 0; o < uint32_t(kSlots); ++o) {
    gains[o] = levels.data() + kArrayStride * o;
    steps[o] = Host(base, scratch + kStepsAt + kArrayStride * o);
  }
  rex::ppc::FPSCRRegister fp = parent.fpscr;
  fp.csr = fp.getcsr();
  const bool start_flush = StartFlush(fp, rng);
  const bool flush_before = (fp.csr & FlushMask()) == FlushMask();
  ComputeRamp(fp, call, Host(base, in), area.data() + kOutMargin, gains, steps);
  SetFlush(g.fpscr, start_flush);
  Guest(kRamp, g, base);
  *ran = true;
  const std::string shape = " (" + std::to_string(nout) + " out, " + std::to_string(nin) + " in, " +
                            std::to_string(frames) + " frames)";
  if (!CompareArea(oarea, area.data(), out_bytes + 2 * kOutMargin, "the output", why) ||
      !CompareArea(Host(base, scratch + kGainsAt), levels.data(), 6 * kArrayStride, "the levels", why)) {
    *why += shape;
    return false;
  }
  if (LdW(Host(base, self + kRampLeft)) != call.left_end || g.r3.u32 != 0) {
    *why = "the ramp count or the result" + shape;
    return false;
  }
  const bool guest_flush = (g.fpscr.csr & FlushMask()) == FlushMask();
  if (guest_flush != (frames > 0 ? false : flush_before)) {
    *why = "the flush mode left" + shape;
    return false;
  }
  return true;
}

// One ramp setup case: native on copies, then the game's code.
bool RunSetupCase(PPCContext& parent, uint8_t* base, uint32_t scratch, Rng& rng, bool* ran, std::string* why) {
  MakeConnection(base, scratch, rng);
  const uint32_t self = scratch + kObjAt;
  const int32_t nout = int32_t(rng.Below(8));  // 0 returns at once; 7 is more than the native code takes
  const int32_t ins[] = {6, 6, 6, 1, 2, 8};
  StW(Host(base, self + kOutCount), uint32_t(nout));
  StW(Host(base, self + kInCount), uint32_t(ins[rng.Below(6)]));
  if (rng.Below(4) == 0) {  // levels already at their targets: steps of 0, no ramp
    std::memcpy(Host(base, scratch + kTargetsAt), Host(base, scratch + kGainsAt), 6 * kArrayStride);
    StW(Host(base, self + kVolume), FBits(1.0f));
  }
  rex::CallFrame frame(parent);
  PPCContext& g = frame.ctx;
  g.r1.u32 = (parent.r1.u32 - 0x1000) & ~0xFu;
  g.r3.u64 = self;
  SetupCall call;
  if (PrepareSetup(base, g, &call) != kOk) return true;
  std::vector<uint8_t> stepcopy(Host(base, scratch + kStepsAt), Host(base, scratch + kStepsAt) + 6 * kArrayStride);
  const uint8_t* targets[kSlots];
  const uint8_t* gains[kSlots];
  uint8_t* steps[kSlots];
  for (uint32_t o = 0; o < uint32_t(kSlots); ++o) {
    targets[o] = Host(base, scratch + kTargetsAt + kArrayStride * o);
    gains[o] = Host(base, scratch + kGainsAt + kArrayStride * o);
    steps[o] = stepcopy.data() + kArrayStride * o;
  }
  const uint32_t left_before = LdW(Host(base, self + kRampLeft));
  rex::ppc::FPSCRRegister fp = parent.fpscr;
  fp.csr = fp.getcsr();
  const bool start_flush = StartFlush(fp, rng);
  const bool set = ComputeSetup(fp, call, targets, gains, steps);
  SetFlush(g.fpscr, start_flush);
  Guest(kRampSetup, g, base);
  *ran = true;
  const std::string shape = " (" + std::to_string(nout) + " outputs, " + (call.six ? "6-level path)" : "general path)");
  if (!CompareArea(Host(base, scratch + kStepsAt), stepcopy.data(), 6 * kArrayStride, "the steps", why)) {
    *why += shape;
    return false;
  }
  if (LdW(Host(base, self + kRampLeft)) != (set ? 64u : left_before) || g.r3.u32 != 0) {
    *why = "the ramp count or the result" + shape;
    return false;
  }
  if ((g.fpscr.csr & FlushMask()) == FlushMask()) {
    *why = "the flush mode left on" + shape;
    return false;
  }
  return true;
}

// One connection mix case: native on a copy of the output, then the game's
// code (during the self-check its ramp and kernels are the game's too).
bool RunConnCase(PPCContext& parent, uint8_t* base, uint32_t scratch, Rng& rng, bool* ran, std::string* why) {
  MakeConnection(base, scratch, rng);
  const uint32_t self = scratch + kObjAt;
  uint8_t* obj = Host(base, self);
  const int32_t shapes[][2] = {{2, 1}, {2, 2}, {2, 2}, {6, 1}, {6, 2}, {6, 2}, {6, 6}, {6, 6}, {6, 6},
                               {1, 1}, {2, 6}, {6, 3}, {4, 2}, {0, 2}, {2, 0}};
  const auto& sh = shapes[rng.Below(15)];
  const int32_t nout = sh[0], nin = sh[1];
  // levels: below the cross threshold off the diagonal half the time; a slot of 0 now and then
  if (rng.Below(2) == 0) {
    for (uint32_t o = 0; o < 6; ++o)
      for (uint32_t i = 0; i < 6; ++i)
        if (o != i) StW(Host(base, scratch + kGainsAt + kArrayStride * o + 4 * i), 0u);
  }
  if (rng.Below(8) == 0) StW(obj + kGains + 4 * (2 + rng.Below(4)), 0u);
  // volume 1 and identity targets (the 6-into-6 kernel's test) some of the time; volume 0 now and then
  const uint32_t vk = rng.Below(10);
  if (vk < 4) {
    StW(obj + kVolume, FBits(1.0f));
    if (rng.Below(3) != 0) {
      for (uint32_t o = 0; o < 6; ++o)
        for (uint32_t i = 0; i < 6; ++i)
          StW(Host(base, scratch + kTargetsAt + kArrayStride * o + 4 * i), FBits(o == i ? 1.0f : 0.0f));
      if (rng.Below(4) == 0)  // one target off
        StW(Host(base, scratch + kTargetsAt + kArrayStride * rng.Below(6) + 4 * rng.Below(6)), FBits(0.5f));
    }
  } else if (vk == 4) {
    StW(obj + kVolume, rng.Below(2) ? 0x80000000u : 0u);
  }
  StW(obj + kRampLeft, 0u);
  uint32_t frames = rng.Below(5) == 0 ? rng.Below(6) : rng.Below(300);
  frames = std::min(frames, (32u * 1024u) / (4u * uint32_t(std::max(1, std::max(nin, nout)))));
  // both 16-byte aligned half the time (the kernels then get groups), else any 4-byte alignment
  const bool aligned = rng.Below(2) == 0;
  const uint32_t in = scratch + kInAt + 64 + (aligned ? 0u : 4u * rng.Below(4));
  const uint32_t out = scratch + kOutAt + kOutMargin + 64 + (aligned ? 0u : 4u * rng.Below(4));
  const uint32_t out_bytes = 4u * uint32_t(std::max(nout, 0)) * frames;
  for (uint32_t b = 0; b < 4u * uint32_t(std::max(nin, 0)) * frames; b += 4) StW(Host(base, in + b), AnyFloat(rng));
  uint8_t* oarea = Host(base, out - kOutMargin);
  for (uint32_t b = 0; b < out_bytes + 2 * kOutMargin; ++b) oarea[b] = uint8_t(0xA5 ^ b);
  for (uint32_t b = 0; b < out_bytes; b += 4) StW(Host(base, out + b), AnyFloat(rng));

  rex::CallFrame frame(parent);
  PPCContext& g = frame.ctx;
  g.r1.u32 = (parent.r1.u32 - 0x1000) & ~0xFu;
  g.r3.u64 = self;
  g.r4.u64 = out;
  g.r5.u64 = in;
  g.r6.s64 = nout;
  g.r7.s64 = nin;
  g.r8.u64 = frames;
  ConnCall call;
  if (PrepareConn(base, g, &call) != kOk) return true;
  std::vector<uint8_t> area(oarea, oarea + out_bytes + 2 * kOutMargin);
  const uint8_t* levels[kSlots];
  for (uint32_t o = 0; o < uint32_t(kSlots); ++o) levels[o] = Host(base, call.gslot[o]);
  rex::ppc::FPSCRRegister fp = parent.fpscr;
  fp.csr = fp.getcsr();
  const bool start_flush = StartFlush(fp, rng);
  ComputeConn(fp, call, Host(base, in), area.data() + kOutMargin, levels);
  const bool native_flush = (fp.csr & FlushMask()) == FlushMask();
  SetFlush(g.fpscr, start_flush);
  Guest(kConn, g, base);
  *ran = true;
  const std::string shape = " (" + std::string(kPathName[call.path]) + ", " + std::to_string(nout) +
                            " out, " + std::to_string(nin) + " in, " + std::to_string(frames) + " frames)";
  if (!CompareArea(oarea, area.data(), out_bytes + 2 * kOutMargin, "the output", why)) {
    *why += shape;
    return false;
  }
  if (g.r3.u32 != 0) {
    *why = "the result" + shape;
    return false;
  }
  if (((g.fpscr.csr & FlushMask()) == FlushMask()) != native_flush) {
    *why = "the flush mode left" + shape;
    return false;
  }
  return true;
}

void RunSelfCheck(PPCContext& ctx, uint8_t* base) {
  auto* ks = rex::system::kernel_state();
  auto* mem = ks ? ks->memory() : nullptr;
  if (!mem || mem->virtual_membase() != base) {
    REXLOG_WARN("cod3: native FMOD mix kernels off: guest memory isn't where the game's code expects it");
    return;
  }
  const uint32_t alloc = mem->SystemHeapAlloc(kScratch + 0x1000);
  if (!alloc) {
    REXLOG_WARN("cod3: native FMOD mix kernels off: no scratch memory for their self-check");
    return;
  }
  const uint32_t scratch = (alloc + 0xFFFu) & ~0xFFFu;
  if (!Plain(base, Range{scratch, uint64_t(scratch) + kScratch})) {
    mem->SystemHeapFree(alloc);
    REXLOG_WARN("cod3: native FMOD mix kernels off: their scratch memory isn't plain guest memory");
    return;
  }
  const uint32_t saved_csr = ctx.fpscr.csr;
  const int64_t t0 = int64_t(NowNs());
  Rng rng{0xD1B54A32D192ED03ull};
  std::string summary;
  int on = 0;
  for (int k = 0; k < kKernelCount; ++k) {
    int cases = 0;
    bool same = true;
    std::string why;
    const int tries = k == kConn ? 400 : 200;  // the connection mix has the most paths
    for (int n = 0; n < tries && same; ++n) {
      bool ran = false;
      if (k == kRamp) same = RunRampCase(ctx, base, scratch, rng, &ran, &why);
      else if (k == kRampSetup) same = RunSetupCase(ctx, base, scratch, rng, &ran, &why);
      else if (k == kConn) same = RunConnCase(ctx, base, scratch, rng, &ran, &why);
      else same = RunCase(ctx, base, scratch, Kernel(k), rng, &ran, &why);
      cases += ran ? 1 : 0;
    }
    const bool ok = same && cases > 0;
    g_ok[k].store(ok, std::memory_order_relaxed);
    g_checks_left[k] = kChecksPerKernel;
    if (!same) {
      REXLOG_ERROR("cod3: native FMOD mix kernels: {} differs from the game's own code ({}); it "
                   "stays on the game's code",
                   kKernelName[k], why);
    }
    char part[96];
    std::snprintf(part, sizeof(part), "%s%s %s (%d)", k ? ", " : "", kKernelName[k],
                  ok ? "same" : "OFF", cases);
    summary += part;
    on += ok ? 1 : 0;
  }
  ctx.fpscr.restoreGuestBits(saved_csr);
  mem->SystemHeapFree(alloc);
  REXLOG_INFO("cod3: native FMOD mix kernels self-check ({:.1f} ms; kernels and cases compared): "
              "{}; {} of {} now run as native code (cod3_native_fmod_mix)",
              double(int64_t(NowNs()) - t0) / 1e6, summary, on, int(kKernelCount));
}

// The switch on and the self-check done (run at the first call; false while
// another thread runs it).
bool Ready(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cod3_native_fmod_mix)) return false;
  if (g_state.load(std::memory_order_acquire) != 2) {
    int expected = 0;
    if (g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> lock(g_lock);
      RunSelfCheck(ctx, base);
      g_state.store(2, std::memory_order_release);
    }
    if (g_state.load(std::memory_order_acquire) != 2) return false;
  }
  return true;
}

// Whether this call is one to run both ways (the first calls, then one in 2048).
bool CheckThisOne(Kernel k) {
  std::lock_guard<std::mutex> lock(g_lock);
  return g_checks_left[k] > 0 || ++g_spot % kSpotEvery == 0;
}

// After a call run both ways: the result into the counters and the log.
void Checked(Kernel k, bool same, const std::string& diff, const std::string& shape) {
  g_checked[k].fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_lock);
  if (!same) {
    g_ok[k].store(false, std::memory_order_relaxed);
    REXLOG_ERROR("cod3: native FMOD mix kernels: {} differs from the game's code in a call ({}; {}); it "
                 "runs the game's code from now on",
                 kKernelName[k], shape, diff);
    return;
  }
  int& left = g_checks_left[k];
  if (left > 0 && --left == 0) {
    REXLOG_INFO("cod3: native FMOD mix kernels ({}): {} calls the same as the game's code; native from "
                "now on",
                kKernelName[k], kChecksPerKernel);
  }
  MaybeReport();
}

void Counted(Kernel k) {
  if ((g_native[k].fetch_add(1, std::memory_order_relaxed) & 0x3FF) == 0) {
    std::lock_guard<std::mutex> lock(g_lock);
    MaybeReport();
  }
}

// A kernel's call: native where Prepare() allows, the first calls of each
// kernel checked against the game's code, else the game's code.
void Run(Kernel k, PPCContext& ctx, uint8_t* base) {
  if (!Ready(ctx, base)) {
    Guest(k, ctx, base);
    return;
  }
  Call call;
  const Why why = Prepare(base, ctx, k, &call);
  if (why != kOk || !g_ok[k].load(std::memory_order_relaxed)) {
    g_why[why].fetch_add(1, std::memory_order_relaxed);
    g_guest.fetch_add(1, std::memory_order_relaxed);
    Guest(k, ctx, base);
    return;
  }
  uint8_t* in = Host(base, call.in);
  uint8_t* out = Host(base, call.out);
  if (CheckThisOne(k)) {
    static thread_local std::vector<uint8_t> mine;
    mine.assign(out, out + call.out_bytes);
    Compute(ctx.fpscr, call, in, mine.data(), mine.data());
    Guest(k, ctx, base);  // the game's result stays; ctx ends as its code leaves it
    std::string diff;
    const bool guest_flush = (ctx.fpscr.csr & FlushMask()) == FlushMask();
    bool same = SameBytes(out, mine.data(), call.out_bytes, kOutChannels[k], &diff);
    if (same && guest_flush != call.flush_end) {
      same = false;
      diff = "the flush mode left";
    }
    Checked(k, same, diff, std::to_string(call.frames) + " frames");
    return;
  }
  const uint64_t t0 = NowNs();
  Compute(ctx.fpscr, call, in, out, out);  // in place: each output read, then written
  g_native_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  g_native_frames.fetch_add(uint64_t(call.frames), std::memory_order_relaxed);
  Counted(k);
}

void ToGuest(Kernel k, Why why, PPCContext& ctx, uint8_t* base) {
  g_why[why].fetch_add(1, std::memory_order_relaxed);
  g_guest.fetch_add(1, std::memory_order_relaxed);
  Guest(k, ctx, base);
}

// The ramped matrix mix: native where PrepareRamp() allows, checked the same way.
void RunRamp(PPCContext& ctx, uint8_t* base) {
  if (!Ready(ctx, base)) {
    Guest(kRamp, ctx, base);
    return;
  }
  RampCall call;
  const Why why = PrepareRamp(base, ctx, &call);
  if (why != kOk || !g_ok[kRamp].load(std::memory_order_relaxed)) {
    ToGuest(kRamp, why, ctx, base);
    return;
  }
  const uint8_t* in = Host(base, call.in);
  uint8_t* out = Host(base, call.out);
  uint8_t* gains[kSlots];
  const uint8_t* steps[kSlots];
  for (uint32_t o = 0; o < uint32_t(kSlots); ++o) {
    gains[o] = Host(base, call.gains[o]);
    steps[o] = Host(base, call.steps[o]);
  }
  const uint32_t out_bytes = 4u * call.nout * call.frames, level_bytes = 4u * call.nin;
  if (CheckThisOne(kRamp)) {
    static thread_local std::vector<uint8_t> mine_out, mine_levels;
    mine_out.assign(out, out + out_bytes);
    mine_levels.resize(size_t(level_bytes) * call.nout);
    uint8_t* mine_gains[kSlots] = {};
    for (uint32_t o = 0; o < call.nout && call.frames > 0; ++o) {
      mine_gains[o] = mine_levels.data() + size_t(level_bytes) * o;
      std::memcpy(mine_gains[o], gains[o], level_bytes);
    }
    const bool flush_before = (ctx.fpscr.csr & FlushMask()) == FlushMask();
    ComputeRamp(ctx.fpscr, call, in, mine_out.data(), mine_gains, steps);
    Guest(kRamp, ctx, base);  // the game's result stays
    std::string diff;
    bool same = CompareArea(out, mine_out.data(), out_bytes, "the output", &diff);
    for (uint32_t o = 0; same && o < call.nout && call.frames > 0; ++o)
      same = CompareArea(gains[o], mine_gains[o], level_bytes, "the levels", &diff);
    if (same && (LdW(Host(base, call.self + kRampLeft)) != call.left_end || ctx.r3.u32 != 0)) {
      same = false;
      diff = "the ramp count or the result";
    }
    const bool guest_flush = (ctx.fpscr.csr & FlushMask()) == FlushMask();
    if (same && guest_flush != (call.frames > 0 ? false : flush_before)) {
      same = false;
      diff = "the flush mode left";
    }
    Checked(kRamp, same, diff,
            std::to_string(call.nout) + " out, " + std::to_string(call.nin) + " in, " +
                std::to_string(call.frames) + " frames");
    return;
  }
  ComputeRamp(ctx.fpscr, call, in, out, gains, steps);
  StW(Host(base, call.self + kRampLeft), call.left_end);
  ctx.r3.u64 = 0;
  Counted(kRamp);
}

// The ramp setup: native where PrepareSetup() allows, checked the same way.
void RunSetup(PPCContext& ctx, uint8_t* base) {
  if (!Ready(ctx, base)) {
    Guest(kRampSetup, ctx, base);
    return;
  }
  SetupCall call;
  const Why why = PrepareSetup(base, ctx, &call);
  if (why != kOk || !g_ok[kRampSetup].load(std::memory_order_relaxed)) {
    ToGuest(kRampSetup, why, ctx, base);
    return;
  }
  const uint8_t* targets[kSlots];
  const uint8_t* gains[kSlots];
  uint8_t* steps[kSlots];
  for (uint32_t o = 0; o < uint32_t(kSlots); ++o) {
    targets[o] = Host(base, call.targets[o]);
    gains[o] = Host(base, call.gains[o]);
    steps[o] = Host(base, call.steps[o]);
  }
  const uint32_t n = call.nout > 0 ? uint32_t(call.nout) : 0u, step_bytes = 4u * call.len;
  uint8_t* left = Host(base, call.self + kRampLeft);
  if (CheckThisOne(kRampSetup)) {
    static thread_local std::vector<uint8_t> mine;
    mine.resize(size_t(step_bytes) * n);
    uint8_t* mine_steps[kSlots] = {};
    for (uint32_t o = 0; o < n; ++o) {
      mine_steps[o] = mine.data() + size_t(step_bytes) * o;
      std::memcpy(mine_steps[o], steps[o], step_bytes);
    }
    const uint32_t left_before = n ? LdW(left) : 0u;
    const bool set = ComputeSetup(ctx.fpscr, call, targets, gains, mine_steps);
    Guest(kRampSetup, ctx, base);  // the game's result stays
    std::string diff;
    bool same = true;
    for (uint32_t o = 0; same && o < n; ++o)
      same = CompareArea(steps[o], mine_steps[o], step_bytes, "the steps", &diff);
    if (same && ((n && LdW(left) != (set ? 64u : left_before)) || ctx.r3.u32 != 0)) {
      same = false;
      diff = "the ramp count or the result";
    }
    if (same && (ctx.fpscr.csr & FlushMask()) == FlushMask()) {
      same = false;
      diff = "the flush mode left on";
    }
    Checked(kRampSetup, same, diff, std::to_string(call.nout) + " outputs");
    return;
  }
  if (ComputeSetup(ctx.fpscr, call, targets, gains, steps)) StW(left, 64u);
  ctx.r3.u64 = 0;
  Counted(kRampSetup);
}


// The connection mix: native where PrepareConn() allows, checked the same way.

void RunConn(PPCContext& ctx, uint8_t* base) {
  if (!Ready(ctx, base)) {
    Guest(kConn, ctx, base);
    return;
  }
  ConnCall call;
  const Why why = PrepareConn(base, ctx, &call);
  if (why != kOk || !g_ok[kConn].load(std::memory_order_relaxed)) {
    ToGuest(kConn, why, ctx, base);
    return;
  }
  const uint8_t* in = Host(base, call.in);
  uint8_t* out = Host(base, call.out);
  const uint8_t* levels[kSlots];
  for (uint32_t o = 0; o < uint32_t(kSlots); ++o) levels[o] = Host(base, call.gslot[o]);
  if (CheckThisOne(kConn)) {
    static thread_local std::vector<uint8_t> mine;
    mine.assign(out, out + call.out_bytes);
    ComputeConn(ctx.fpscr, call, in, mine.data(), levels);
    const bool native_flush = (ctx.fpscr.csr & FlushMask()) == FlushMask();
    Guest(kConn, ctx, base);  // the game's result stays
    std::string diff;
    bool same = CompareArea(out, mine.data(), call.out_bytes, "the output", &diff);
    if (same && ctx.r3.u32 != 0) {
      same = false;
      diff = "the result";
    }
    if (same && ((ctx.fpscr.csr & FlushMask()) == FlushMask()) != native_flush) {
      same = false;
      diff = "the flush mode left";
    }
    Checked(kConn, same, diff,
            std::string(kPathName[call.path]) + ", " + std::to_string(call.nout) + " out, " +
                std::to_string(call.nin) + " in, " + std::to_string(call.frames) + " frames");
    return;
  }
  ComputeConn(ctx.fpscr, call, in, out, levels);
  ctx.r3.u64 = 0;
  g_paths[call.path].fetch_add(1, std::memory_order_relaxed);
  Counted(kConn);
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_fmod_mix_report, ReportCommand, "cod3/Performance",
                            "cod3_native_fmod_mix_report: how many of FMOD's mix kernel calls ran "
                            "as native code, by kernel, written to the log");

REX_HOOK_RAW(sub_823BD5D8) { Run(kMonoStereo, ctx, base); }
REX_HOOK_RAW(sub_823BD6C0) { Run(kStereoStereo, ctx, base); }
REX_HOOK_RAW(sub_823BD7C0) { Run(kMonoSix, ctx, base); }
REX_HOOK_RAW(sub_823BD998) { Run(kSixSix, ctx, base); }
REX_HOOK_RAW(sub_823AF9E0) { RunRamp(ctx, base); }
REX_HOOK_RAW(sub_823AFB68) { RunSetup(ctx, base); }
REX_HOOK_RAW(sub_823B07C8) { RunConn(ctx, base); }
