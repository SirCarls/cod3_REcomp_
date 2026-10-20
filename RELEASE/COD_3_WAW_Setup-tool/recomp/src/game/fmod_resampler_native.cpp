// cod3 - [native-resampler] FMOD's linear resampler as native code.
//
// FMOD Ex's software mixer resamples every playing sound through one function,
// sub_823BA518 (cod3mp sub_82389BA0, the same 3,119 instructions apart from
// where its constants sit): linear interpolation from a source buffer into a
// float buffer, with a 64-bit fixed point position (the high word the source
// frame, the low word the fraction) that a 64-bit speed is added to after each
// output frame:
//
//   resample(out r3, frames r4, src r5, format r6, &position r7, &speed r8,
//            channels r9)
//     - the speed's high word must lie between two of FMOD's words (the
//       game's code otherwise logs, then writes silence or a zero speed);
//     - format 1..5 is PCM8, PCM16, PCM24 (little-endian, sign-extended),
//       PCM32 and float; each sample is converted to float and scaled (1/128,
//       1/32768, 2^-23, -2^-31 (sic: FMOD's PCM32 scale is negative), 1);
//     - out = s0 * (1 - f) + s1 * f, with s0 / s1 the frame at the position
//       and the next, f the fraction times 2^-32.
//
// The compiler wrote 13 versions of that loop (mono, stereo and any channel
// count, by format), and they don't all round the same: the scalar ones use
// one of two fused orders ("A": fma(s1, f, s0 * (1 - f)); "B": fma(s0, 1 - f,
// s1 * f); each an fmadds, a double fma rounded to float, as the generated
// code computes it), and the VMX ones (mono PCM8 / PCM16 / float and stereo
// PCM16 / float, 16- or 32-byte aligned output) multiply and add separately
// with the vector unit's denormal flush on. Which version, and which order for
// which channel and frame, is what the compiler's scheduling left (read out of
// the generated code with a symbolic run of each loop):
//   - mono 8 / 16 / float: B up to a 16-byte boundary, then 8 frames at a
//     time in VMX, the rest B; mono 24 and 32: B;
//   - stereo 8: in groups of 4 frames left A, right B (the 4th frame's right
//     A), the rest A A; stereo 16 / float: B B up to a 32-byte boundary, 4
//     frames at a time in VMX, the rest A A;
//   - any other channel count (stereo too for 24 and 32): per frame, channels
//     in groups of 4 then one at a time; 8, 16 and float A everywhere, 24 and
//     32 A A A B in each group and B for the singles.
// The native version does exactly that: the same operations in the same order
// for each output (scalar steps as the generated code's double operations
// rounded to float, the vector steps with the same SIMD calls, the unsigned
// fraction conversion included), the denormal flush switched the same way and
// left as the game's code leaves it, the position stepped the same way.
//
// It runs only where that is provably what the game's code does, decided
// before anything is written: the speed in range, a format and channel count
// it knows (1..32 channels), 1 to 65536 frames, the output 4-byte aligned (8
// for stereo 16 / float, whose vector stores need it) and plain guest memory,
// every source frame the call reads plain and found without the 32-bit
// address wrapping, and nothing the call writes (the output, the position)
// over what it reads (the source, the position, the speed). A float source
// with a NaN or an infinity in the frames it reads goes to the game's code
// (which NaN comes out of two NaN operands depends on the instruction the
// compiler picked; see fmod_native.cpp). Everything else goes to the game's
// code.
//
// At the first call a self-check runs the game's code and the native version
// on scratch guest memory for every version (random positions, speeds, frame
// counts and output alignments; random samples, for floats denormals, zeros
// and large values too) and compares the output, the bytes around it, the
// position and the flush state; a version that differs stays on the game's
// code. In play the first 16 calls of each version are run both ways too (the
// game's result kept), then one call in 2048. cod3_native_resampler (on)
// switches it off at any time; cod3_native_resampler_report writes the counts
// to the log.
//
// Both programs: tools/mp_aliases.py points sub_823BA518 at cod3mp's
// sub_82389BA0 and COD3_ADDR gives the constants' and FMOD words' addresses
// (mp/config/campaign_map.tsv).

#include <algorithm>
#include <atomic>
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

REXCVAR_DEFINE_BOOL(cod3_native_resampler, true, "cod3/Performance",
                    "FMOD's resampler (every playing sound's pitch and rate change in the mixer) "
                    "runs as native code where that does exactly what the game's own code does "
                    "(checked at start and on its first calls; off: the translated Xbox 360 code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_823BA518);  // FMOD's linear resampler (cod3mp sub_82389BA0)

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

// ---------------------------------------------------------------------------
// The game's constants and FMOD's words (cod3mp's sit elsewhere)
// ---------------------------------------------------------------------------

constexpr uint32_t kOneAddr = COD3_ADDR(0x820014D0, 0x82001570);       // 1.0
constexpr uint32_t kHalfAddr = COD3_ADDR(0x820014D8, 0x82001578);      // 0.5 (the VMX fraction)
constexpr uint32_t kFracScaleAddr = COD3_ADDR(0x82018B28, 0x82019FC4); // 2^-32 (the fraction)
constexpr uint32_t kScale8Addr = COD3_ADDR(0x8207C868, 0x8208552C);    // 1/128
constexpr uint32_t kScale16Addr = COD3_ADDR(0x8207C768, 0x820853F4);   // 1/32768
constexpr uint32_t kScale24Addr = COD3_ADDR(0x82017738, 0x82017438);   // 2^-23
constexpr uint32_t kScale32Addr = COD3_ADDR(0x82060284, 0x8205ECB4);   // -2^-31
constexpr uint32_t kMinSpeedAddr = COD3_ADDR(0x829AFBDC, 0x82B2BD04);  // s32: the speed's lowest high word
constexpr uint32_t kMaxSpeedAddr = COD3_ADDR(0x825CC054, 0x825D1564);  // s32: its highest

// The VMX conversions' scales as the generated code builds them (vcfux 31,
// vcfsx 7 and 15: set1_epi32 of the power of two's bits).
constexpr uint32_t kVcf31 = 0x30000000u;  // 2^-31
constexpr uint32_t kVcf7 = 0x3C000000u;   // 2^-7
constexpr uint32_t kVcf15 = 0x38000000u;  // 2^-15

constexpr int32_t kMaxFrames = 65536;
constexpr int32_t kMaxChannels = 32;

// ---------------------------------------------------------------------------
// The versions of the loop
// ---------------------------------------------------------------------------

enum Shape : int {
  kMono8 = 0, kStereo8, kMulti8,
  kMono16, kStereo16, kMulti16,
  kMono24, kMulti24,
  kMono32, kMulti32,
  kMonoF, kStereoF, kMultiF,
  kShapeCount
};
constexpr const char* kShapeName[kShapeCount] = {
    "mono 8-bit",  "stereo 8-bit",  "multichannel 8-bit",  "mono 16-bit", "stereo 16-bit",
    "multichannel 16-bit", "mono 24-bit", "24-bit (2+ channels)", "mono 32-bit",
    "32-bit (2+ channels)", "mono float", "stereo float", "multichannel float"};

Shape ShapeOf(int32_t format, int32_t channels) {
  switch (format) {
    case 1: return channels == 1 ? kMono8 : channels == 2 ? kStereo8 : kMulti8;
    case 2: return channels == 1 ? kMono16 : channels == 2 ? kStereo16 : kMulti16;
    case 3: return channels == 1 ? kMono24 : kMulti24;
    case 4: return channels == 1 ? kMono32 : kMulti32;
    default: return channels == 1 ? kMonoF : channels == 2 ? kStereoF : kMultiF;
  }
}

inline bool IsVmxStereo(Shape s) { return s == kStereo16 || s == kStereoF; }

// One call, checked and laid out.
struct Call {
  uint32_t out = 0, src = 0, posp = 0, speedp = 0;
  int32_t frames = 0, format = 0, channels = 0;
  Shape shape = kMono8;
  uint32_t bps = 0;  // bytes per sample
  uint32_t bpf = 0;  // bytes per frame
  uint64_t pos = 0, speed = 0;
  // the game's constants, as the code reads them (lfs: the float, widened)
  double one = 0, half = 0, frac_scale = 0, scale = 0;
  uint32_t one_bits = 0, half_bits = 0;
  // results
  std::vector<uint32_t> result;  // frames * channels float bits
  uint64_t pos_end = 0;
  bool flush_end = false;  // the denormal flush left on (the last float step was a VMX one)
};

enum Why : int {
  kOk = 0,
  kSpeed,    // the speed outside FMOD's range (the game's code logs and silences)
  kShapeOut, // a format, channel count or frame count the native code doesn't take
  kAlign,    // the output not aligned as the native code needs
  kMemory,   // an address that isn't plain guest memory, or the address wraps
  kOverlaps, // what the call writes over what it reads
  kSpecial,  // a NaN or an infinity in a float source
  kWhyCount
};
constexpr const char* kWhyName[kWhyCount] = {"ok",     "speed",   "shape", "alignment",
                                             "memory", "overlap", "nan or infinity"};

// The call's arguments as the code reads them, and whether the native code
// takes it (everything checked before anything is written).
Why Prepare(uint8_t* base, uint32_t out, int32_t frames, uint32_t src, int32_t format,
            uint32_t posp, uint32_t speedp, int32_t channels, Call* c) {
  c->out = out;
  c->frames = frames;
  c->src = src;
  c->format = format;
  c->posp = posp;
  c->speedp = speedp;
  c->channels = channels;
  const Range pos_r{posp, uint64_t(posp) + 8}, speed_r{speedp, uint64_t(speedp) + 8};
  for (const Range& r : {pos_r, speed_r, Range{kMinSpeedAddr, kMinSpeedAddr + 4ull},
                         Range{kMaxSpeedAddr, kMaxSpeedAddr + 4ull}}) {
    if (!Plain(base, r)) return kMemory;
  }
  const int32_t speed_hi = int32_t(LdW(Host(base, speedp)));
  if (speed_hi < int32_t(LdW(Host(base, kMinSpeedAddr))) ||
      speed_hi > int32_t(LdW(Host(base, kMaxSpeedAddr))))
    return kSpeed;  // cmpw / blt, cmpw / ble: the game's code handles it
  if (format < 1 || format > 5 || frames < 1 || frames > kMaxFrames || channels < 1 ||
      channels > kMaxChannels)
    return kShapeOut;
  c->shape = ShapeOf(format, channels);
  c->bps = format == 1 ? 1 : format == 2 ? 2 : format == 3 ? 3 : 4;
  c->bpf = c->bps * uint32_t(channels);
  if ((out & 3) != 0 || (IsVmxStereo(c->shape) && (out & 7) != 0)) return kAlign;

  const Range out_r{out, uint64_t(out) + 4ull * uint32_t(frames) * uint32_t(channels)};
  if (!Plain(base, out_r)) return kMemory;
  if (Overlap(out_r, pos_r) || Overlap(out_r, speed_r) || Overlap(pos_r, speed_r)) return kOverlaps;

  // Every frame's source position (the high word), as the code steps it.
  c->pos = LdD(Host(base, posp));
  c->speed = LdD(Host(base, speedp));
  uint32_t lo_hi = 0xFFFFFFFFu, hi_hi = 0;
  uint64_t p = c->pos;
  for (int32_t i = 0; i < frames; ++i) {
    const uint32_t h = uint32_t(p >> 32);
    lo_hi = std::min(lo_hi, h);
    hi_hi = std::max(hi_hi, h);
    p += c->speed;
  }
  c->pos_end = p;
  // Each frame reads [src + h * bpf, src + (h + 2) * bpf); the code's 32-bit
  // sums equal these only if the last one doesn't pass 2^32.
  const Range src_r{uint64_t(src) + uint64_t(lo_hi) * c->bpf,
                    uint64_t(src) + (uint64_t(hi_hi) + 2) * c->bpf};
  if (!Plain(base, src_r)) return kMemory;
  if (Overlap(out_r, src_r) || Overlap(pos_r, src_r)) return kOverlaps;

  const Range consts[] = {{kOneAddr, kOneAddr + 4ull},          {kHalfAddr, kHalfAddr + 4ull},
                          {kFracScaleAddr, kFracScaleAddr + 4ull}, {kScale8Addr, kScale8Addr + 4ull},
                          {kScale16Addr, kScale16Addr + 4ull},   {kScale24Addr, kScale24Addr + 4ull},
                          {kScale32Addr, kScale32Addr + 4ull}};
  for (const Range& r : consts) {
    if (!Plain(base, r)) return kMemory;
  }
  c->one_bits = LdW(Host(base, kOneAddr));
  c->half_bits = LdW(Host(base, kHalfAddr));
  c->one = double(BitsF(c->one_bits));
  c->half = double(BitsF(c->half_bits));
  c->frac_scale = double(BitsF(LdW(Host(base, kFracScaleAddr))));
  const uint32_t scale_addr = format == 1   ? kScale8Addr
                              : format == 2 ? kScale16Addr
                              : format == 3 ? kScale24Addr
                                            : kScale32Addr;
  c->scale = format == 5 ? 1.0 : double(BitsF(LdW(Host(base, scale_addr))));

  if (format == 5) {  // a float source: no NaN or infinity in what it reads
    const uint8_t* s = Host(base, uint32_t(src_r.begin));
    const uint64_t n = (src_r.end - src_r.begin) / 4;
    for (uint64_t i = 0; i < n; ++i) {
      if ((LdW(s + 4 * i) & 0x7F800000u) == 0x7F800000u) return kSpecial;
    }
  }
  return kOk;
}

// ---------------------------------------------------------------------------
// The arithmetic, step by step as the generated code does it
// ---------------------------------------------------------------------------

inline double Rs(double v) { return double(float(v)); }  // frsp, and every single-precision result

// A sample: lbz / lhz / the 24-bit assembly / lwz, sign-extended, fcfid,
// frsp, fmuls by the format's scale; lfs for floats.
inline double Sample(const uint8_t* p, int32_t format, double scale) {
  int64_t v;
  switch (format) {
    case 1: v = int64_t(int8_t(p[0])); break;
    case 2: v = int64_t(int16_t(LdH(p))); break;
    case 3: {
      // lbz +2, +1, +0; rotlwi 8, or, rlwinm 8,0,23, or, rlwinm 8,0,23; srawi 8
      const uint32_t w = (uint32_t(p[2]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[0]) << 8);
      v = int64_t(int32_t(w) >> 8);
      break;
    }
    case 4: v = int64_t(int32_t(LdW(p))); break;
    default: return double(BitsF(LdW(p)));
  }
  return Rs(Rs(double(v)) * scale);
}

// The scalar fraction: fcfid of the low word, frsp, fmuls by 2^-32.
inline double Frac(uint32_t lo, double frac_scale) {
  return Rs(Rs(double(int64_t(lo))) * frac_scale);
}

// The two fused orders (fmadds: a double fma rounded to float).
inline uint32_t FormA(double s0, double s1, double f, double omf) {  // s1 * f + s0 * (1 - f)
  return FBits(float(std::fma(s1, f, Rs(s0 * omf))));
}
inline uint32_t FormB(double s0, double s1, double f, double omf) {  // s0 * (1 - f) + s1 * f
  return FBits(float(std::fma(s0, omf, Rs(s1 * f))));
}

// Four outputs the VMX way: vcfux 31 of the fractions, times 0.5; 1 - f;
// s1 * f; s0 * (1 - f) + that (vmaddfp: a multiply, then an add). s0 / s1 are
// the four lanes' samples as the VMX code converts them.
inline void Vector4(const uint32_t lo[4], simde__m128 s0, simde__m128 s1, simde__m128 one4,
                    simde__m128 half4, uint32_t out[4]) {
  const simde__m128i lov = simde_mm_set_epi32(int(lo[3]), int(lo[2]), int(lo[1]), int(lo[0]));
  const simde__m128 fr = simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(lov),
                                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int(kVcf31))));
  const simde__m128 f = simde_mm_mul_ps(fr, half4);
  const simde__m128 omf = simde_mm_sub_ps(one4, f);
  const simde__m128 p = simde_mm_mul_ps(s1, f);
  const simde__m128 r = simde_mm_add_ps(simde_mm_mul_ps(s0, omf), p);
  alignas(16) float lanes[4];
  simde_mm_store_ps(lanes, r);
  for (int k = 0; k < 4; ++k) out[k] = FBits(lanes[k]);
}

// The VMX code's samples: vcfsx 7 / 15 of the sign-extended ints, or the
// floats as they are.
inline simde__m128 VecSamples(const int32_t ints[4], const uint32_t bits[4], int32_t format) {
  if (format == 5) {
    return simde_mm_castsi128_ps(
        simde_mm_set_epi32(int(bits[3]), int(bits[2]), int(bits[1]), int(bits[0])));
  }
  const simde__m128i v = simde_mm_set_epi32(ints[3], ints[2], ints[1], ints[0]);
  return simde_mm_mul_ps(simde_mm_cvtepi32_ps(v),
                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int(format == 1 ? kVcf7 : kVcf15))));
}

inline int32_t RawInt(const uint8_t* p, int32_t format) {
  return format == 1 ? int32_t(int8_t(p[0])) : int32_t(int16_t(LdH(p)));
}

// The whole call into c->result and c->pos_end, the flush mode switched as
// the game's code switches it (fpscr: the context's, or the check's copy).
void Compute(uint8_t* base, rex::ppc::FPSCRRegister& fpscr, Call* c) {
  const int32_t n = c->channels;
  c->result.assign(size_t(c->frames) * size_t(n), 0u);
  uint32_t* out = c->result.data();
  const uint8_t* src = Host(base, c->src);  // src + h * bpf: plain and without wrapping (Prepare)
  uint64_t p = c->pos;
  const double one = c->one, fs = c->frac_scale, sc = c->scale;
  const int32_t fmt = c->format;
  const uint32_t bps = c->bps, bpf = c->bpf;
  auto frame = [&](uint64_t pos) { return src + uint64_t(uint32_t(pos >> 32)) * bpf; };
  int32_t i = 0;  // output frames done
  c->flush_end = false;

  fpscr.disableFlushMode();  // the setup's lfs
  switch (c->shape) {
    case kMono8:
    case kMono16:
    case kMonoF:
    case kStereo16:
    case kStereoF: {
      const bool stereo = IsVmxStereo(c->shape);
      int32_t head = 0;
      const uint32_t mask = stereo ? 31u : 15u;
      if ((c->out & mask) != 0) {
        head = int32_t((((c->out + mask) & ~mask) - c->out) >> (stereo ? 3 : 2));
        head = std::min(head, c->frames);
      }
      for (; i < head; ++i, p += c->speed) {
        const uint8_t* fr = frame(p);
        const double f = Frac(uint32_t(p), fs), omf = Rs(one - f);
        for (int32_t ch = 0; ch < n; ++ch) {
          out[size_t(i) * n + ch] =
              FormB(Sample(fr + ch * bps, fmt, sc), Sample(fr + bpf + ch * bps, fmt, sc), f, omf);
        }
      }
      const int32_t rest = c->frames - head;
      const int32_t groups = stereo ? rest >> 2 : rest >> 3;
      const simde__m128 one4 = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(c->one_bits)));
      const simde__m128 half4 = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(c->half_bits)));
      if (groups > 0) fpscr.enableFlushMode();
      // Four outputs a vector: mono 4 frames, stereo 2 frames (left, right).
      const int32_t vec_frames = groups * (stereo ? 4 : 8);
      for (int32_t v = 0; v < vec_frames; v += stereo ? 2 : 4) {
        uint32_t lo[4], bits0[4], bits1[4];
        int32_t i0[4], i1[4];
        for (int k = 0; k < 4; ++k) {
          const int32_t fidx = stereo ? (k >> 1) : k;  // frame within this vector
          const int32_t ch = stereo ? (k & 1) : 0;
          uint64_t pk = p;
          for (int32_t s = 0; s < fidx; ++s) pk += c->speed;
          lo[k] = uint32_t(pk);
          const uint8_t* fr = frame(pk);
          const uint8_t* a0 = fr + ch * bps;
          const uint8_t* a1 = fr + bpf + ch * bps;
          if (fmt == 5) {
            bits0[k] = LdW(a0);
            bits1[k] = LdW(a1);
          } else {
            i0[k] = RawInt(a0, fmt);
            i1[k] = RawInt(a1, fmt);
          }
        }
        uint32_t r[4];
        Vector4(lo, VecSamples(i0, bits0, fmt), VecSamples(i1, bits1, fmt), one4, half4, r);
        for (int k = 0; k < 4; ++k) out[size_t(i) * n + k] = r[k];  // n outputs a frame, in order
        const int32_t adv = stereo ? 2 : 4;
        for (int32_t s = 0; s < adv; ++s) p += c->speed;
        i += adv;
      }
      if (i < c->frames) {
        fpscr.disableFlushMode();
        for (; i < c->frames; ++i, p += c->speed) {
          const uint8_t* fr = frame(p);
          const double f = Frac(uint32_t(p), fs), omf = Rs(one - f);
          for (int32_t ch = 0; ch < n; ++ch) {
            const double s0 = Sample(fr + ch * bps, fmt, sc), s1 = Sample(fr + bpf + ch * bps, fmt, sc);
            out[size_t(i) * n + ch] = stereo ? FormA(s0, s1, f, omf) : FormB(s0, s1, f, omf);
          }
        }
      } else {
        c->flush_end = groups > 0;
      }
      break;
    }
    case kStereo8: {
      const int32_t grouped = (c->frames >> 2) << 2;
      for (; i < c->frames; ++i, p += c->speed) {
        const uint8_t* fr = frame(p);
        const double f = Frac(uint32_t(p), fs), omf = Rs(one - f);
        const double l0 = Sample(fr, 1, sc), r0 = Sample(fr + 1, 1, sc);
        const double l1 = Sample(fr + 2, 1, sc), r1 = Sample(fr + 3, 1, sc);
        out[size_t(i) * 2] = FormA(l0, l1, f, omf);
        const bool right_b = i < grouped && (i & 3) != 3;
        out[size_t(i) * 2 + 1] = right_b ? FormB(r0, r1, f, omf) : FormA(r0, r1, f, omf);
      }
      break;
    }
    case kMono24:
    case kMono32: {
      for (; i < c->frames; ++i, p += c->speed) {
        const uint8_t* fr = frame(p);
        const double f = Frac(uint32_t(p), fs), omf = Rs(one - f);
        out[i] = FormB(Sample(fr, fmt, sc), Sample(fr + bpf, fmt, sc), f, omf);
      }
      break;
    }
    case kMulti8:
    case kMulti16:
    case kMultiF:
    case kMulti24:
    case kMulti32: {
      // Channels in groups of 4 while one remains past the first three of the
      // next group (do-while from 0, only with 4 or more), then one at a time.
      const bool b_last = c->shape == kMulti24 || c->shape == kMulti32;
      const int32_t grouped = n >= 4 ? (n >> 2) << 2 : 0;
      for (; i < c->frames; ++i, p += c->speed) {
        const uint8_t* fr = frame(p);
        const double f = Frac(uint32_t(p), fs), omf = Rs(one - f);
        for (int32_t ch = 0; ch < n; ++ch) {
          const double s0 = Sample(fr + ch * bps, fmt, sc), s1 = Sample(fr + bpf + ch * bps, fmt, sc);
          const bool form_b = b_last && (ch >= grouped || (ch & 3) == 3);
          out[size_t(i) * n + ch] = form_b ? FormB(s0, s1, f, omf) : FormA(s0, s1, f, omf);
        }
      }
      break;
    }
    default:
      break;
  }
  c->pos_end = p;
  if (c->flush_end) fpscr.enableFlushMode();
  else fpscr.disableFlushMode();
}

// The native results into guest memory.
void Commit(uint8_t* base, const Call& c) {
  uint8_t* out = Host(base, c.out);
  for (size_t k = 0; k < c.result.size(); ++k) StW(out + 4 * k, c.result[k]);
  StD(Host(base, c.posp), c.pos_end);
}

// What the game's code wrote compared with the native results (output and
// position); the first difference into *why.
bool SameAsGuest(uint8_t* base, const Call& c, std::string* why) {
  const uint8_t* out = Host(base, c.out);
  for (size_t k = 0; k < c.result.size(); ++k) {
    const uint32_t g = LdW(out + 4 * k);
    if (g != c.result[k]) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "output %zu (frame %zu): the game's code 0x%08X, native 0x%08X",
                    k, k / size_t(c.channels), g, c.result[k]);
      *why = buf;
      return false;
    }
  }
  const uint64_t gp = LdD(Host(base, c.posp));
  if (gp != c.pos_end) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "position: the game's code 0x%016llX, native 0x%016llX",
                  static_cast<unsigned long long>(gp), static_cast<unsigned long long>(c.pos_end));
    *why = buf;
    return false;
  }
  return true;
}

constexpr uint32_t FlushMask() { return uint32_t(rex::ppc::FPSCRRegister::FlushMask); }

// ---------------------------------------------------------------------------
// The checks and the counters
// ---------------------------------------------------------------------------

constexpr int kChecksPerShape = 16;
constexpr uint64_t kSpotEvery = 2048;  // after those, one call in this many is checked too

std::mutex g_lock;
std::atomic<int> g_state{0};  // 0 the self-check not run yet, 1 running, 2 done
std::atomic<bool> g_ok[kShapeCount]{};
int g_checks_left[kShapeCount];
uint64_t g_spot = 0;
std::atomic<uint64_t> g_native[kShapeCount]{}, g_checked[kShapeCount]{}, g_guest{0};
std::atomic<uint64_t> g_why[kWhyCount]{};
std::atomic<uint64_t> g_native_ns{0}, g_native_frames{0};
std::chrono::steady_clock::time_point g_next_report{};

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void Report() {
  std::string per;
  for (int s = 0; s < kShapeCount; ++s) {
    const uint64_t n = g_native[s].load(std::memory_order_relaxed);
    const uint64_t k = g_checked[s].load(std::memory_order_relaxed);
    if (!n && !k) continue;
    char part[128];
    std::snprintf(part, sizeof(part), "%s%s %llu (%llu compared)%s", per.empty() ? "" : ", ",
                  kShapeName[s], static_cast<unsigned long long>(n),
                  static_cast<unsigned long long>(k), g_ok[s].load() ? "" : " OFF");
    per += part;
  }
  std::string why;
  for (int k = 1; k < kWhyCount; ++k) {
    const uint64_t v = g_why[k].load(std::memory_order_relaxed);
    if (v) why += std::string(why.empty() ? "" : ", ") + kWhyName[k] + " " + std::to_string(v);
  }
  const uint64_t frames = g_native_frames.load(std::memory_order_relaxed);
  REXLOG_INFO("cod3: native FMOD resampler so far: calls run natively by version: {}; {} by the "
              "game's code{}{}{}; native {:.1f} ns a frame",
              per.empty() ? "none" : per, g_guest.load(), why.empty() ? "" : " (", why,
              why.empty() ? "" : ")",
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

constexpr uint32_t kScratch = 256 * 1024;
constexpr uint32_t kSrcAt = 0;              // the source, 128 KB
constexpr uint32_t kOutAt = 128 * 1024;     // the output, 64 KB (with the bytes around it)
constexpr uint32_t kPosAt = 200 * 1024;     // the position
constexpr uint32_t kSpeedAt = 200 * 1024 + 64;
constexpr uint32_t kOutMargin = 64;         // bytes compared on each side of the output

// A float sample of every kind but NaN and infinity.
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

// One case on the scratch block: the game's code, then the native version on
// the same inputs. false (with *why) if they differ.
bool RunCase(PPCContext& parent, uint8_t* base, uint32_t scratch, Shape shape, int32_t format,
             int32_t channels, Rng& rng, int32_t min_hi, int32_t max_hi, bool* ran, std::string* why) {
  uint8_t* host = Host(base, scratch);
  const uint32_t bps = format == 1 ? 1 : format == 2 ? 2 : format == 3 ? 3 : 4;
  const uint32_t bpf = bps * uint32_t(channels);
  // Frames, the output's alignment, the speed and the start.
  const int32_t frames = int32_t(rng.Below(5) == 0 ? 1 + rng.Below(8) : 1 + rng.Below(300));
  const uint32_t align = IsVmxStereo(shape) ? 8u * rng.Below(4) : 4u * rng.Below(4);
  const uint32_t out = scratch + kOutAt + kOutMargin + 64 + align;
  const int32_t lo = std::max(min_hi, 0), hi = std::min(max_hi, 2);
  if (hi < lo) return true;
  uint64_t speed;
  const uint32_t sk = rng.Below(6);
  if (sk == 0) speed = uint64_t(1) << 32;                       // 1.0
  else if (sk == 1) speed = uint64_t(1) << 31;                  // 0.5
  else speed = (uint64_t(uint32_t(lo + int32_t(rng.Below(uint32_t(hi - lo + 1))))) << 32) |
               uint32_t(rng.Next());
  if (uint32_t(speed >> 32) == 0 && rng.Below(4) == 0) speed &= 0xFFFFull;  // very slow
  const uint64_t pos = (uint64_t(rng.Below(64)) << 32) | uint32_t(rng.Next());
  // The source frames it reads (at most 64 + 300 * 3 + 2 of up to 24 bytes).
  const uint64_t last = ((pos + uint64_t(frames) * speed) >> 32) + 2;
  if (last * bpf > 128 * 1024) return true;
  for (uint64_t k = 0; k < last * bpf; k += 4) {
    if (format == 5) StW(host + kSrcAt + k, AnyFloat(rng));
    else StW(host + kSrcAt + k, uint32_t(rng.Next()));
  }
  // The output area and its margins: a pattern.
  const uint32_t out_len = 4u * uint32_t(frames) * uint32_t(channels);
  uint8_t* oarea = Host(base, out - kOutMargin);
  for (uint32_t k = 0; k < out_len + 2 * kOutMargin; ++k) oarea[k] = uint8_t(0xA5 ^ k);
  StD(host + kPosAt, pos);
  StD(host + kSpeedAt, speed);

  Call call;
  const Why w = Prepare(base, out, frames, scratch + kSrcAt, format, scratch + kPosAt,
                        scratch + kSpeedAt, channels, &call);
  if (w != kOk) return true;  // not a native case (shouldn't happen here)
  rex::CallFrame frame(parent);
  PPCContext& g = frame.ctx;
  g.r1.u32 = (parent.r1.u32 - 0x1000) & ~0xFu;
  // The native version first, then the game's code, each from the real control
  // word as the other left it (the cached copies resynced: the generated code
  // only switches the flush mode when its copy says it differs).
  rex::ppc::FPSCRRegister mine = parent.fpscr;
  mine.csr = mine.getcsr();
  Compute(base, mine, &call);
  g.fpscr.csr = g.fpscr.getcsr();
  g.r3.u64 = out;
  g.r4.s64 = frames;
  g.r5.u64 = scratch + kSrcAt;
  g.r6.s64 = format;
  g.r7.u64 = scratch + kPosAt;
  g.r8.u64 = scratch + kSpeedAt;
  g.r9.s64 = channels;
  __imp__sub_823BA518(g, base);
  *ran = true;
  if (!SameAsGuest(base, call, why)) return false;
  for (uint32_t k = 0; k < kOutMargin; ++k) {
    if (oarea[k] != uint8_t(0xA5 ^ k) ||
        oarea[kOutMargin + out_len + k] != uint8_t(0xA5 ^ (kOutMargin + out_len + k))) {
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

void RunSelfCheck(PPCContext& ctx, uint8_t* base) {
  auto* ks = rex::system::kernel_state();
  auto* mem = ks ? ks->memory() : nullptr;
  if (!mem || mem->virtual_membase() != base) {
    REXLOG_WARN("cod3: native FMOD resampler off: guest memory isn't where the game's code expects it");
    return;
  }
  const uint32_t alloc = mem->SystemHeapAlloc(kScratch + 0x1000);
  if (!alloc) {
    REXLOG_WARN("cod3: native FMOD resampler off: no scratch memory for its self-check");
    return;
  }
  const uint32_t scratch = (alloc + 0xFFFu) & ~0xFFFu;
  if (!Plain(base, Range{scratch, uint64_t(scratch) + kScratch}) ||
      !Plain(base, Range{kMinSpeedAddr, kMinSpeedAddr + 4ull}) ||
      !Plain(base, Range{kMaxSpeedAddr, kMaxSpeedAddr + 4ull})) {
    mem->SystemHeapFree(alloc);
    REXLOG_WARN("cod3: native FMOD resampler off: its scratch memory isn't plain guest memory");
    return;
  }
  const int32_t min_hi = int32_t(LdW(Host(base, kMinSpeedAddr)));
  const int32_t max_hi = int32_t(LdW(Host(base, kMaxSpeedAddr)));
  const uint32_t saved_csr = ctx.fpscr.csr;
  const int64_t t0 = int64_t(NowNs());
  Rng rng{0x9E3779B97F4A7C15ull};
  std::string summary;
  int on = 0;
  for (int s = 0; s < kShapeCount; ++s) {
    const int32_t format = s < 3 ? 1 : s < 6 ? 2 : s < 8 ? 3 : s < 10 ? 4 : 5;
    int cases = 0;
    bool same = true;
    std::string why;
    for (int k = 0; k < 160 && same; ++k) {
      int32_t channels;
      switch (Shape(s)) {
        case kMono8: case kMono16: case kMono24: case kMono32: case kMonoF: channels = 1; break;
        case kStereo8: case kStereo16: case kStereoF: channels = 2; break;
        case kMulti24: case kMulti32: {
          const int32_t pick[] = {2, 3, 4, 5, 6, 8};
          channels = pick[rng.Below(6)];
          break;
        }
        default: {
          const int32_t pick[] = {3, 4, 5, 6, 7, 8};
          channels = pick[rng.Below(6)];
          break;
        }
      }
      bool ran = false;
      same = RunCase(ctx, base, scratch, Shape(s), format, channels, rng, min_hi, max_hi, &ran, &why);
      cases += ran ? 1 : 0;
      if (!same) {
        why += " (" + std::to_string(channels) + " channels)";
      }
    }
    const bool ok = same && cases > 0;
    g_ok[s].store(ok, std::memory_order_relaxed);
    g_checks_left[s] = kChecksPerShape;
    if (!same) {
      REXLOG_ERROR("cod3: native FMOD resampler: {} differs from the game's own code ({}); it stays "
                   "on the game's code",
                   kShapeName[s], why);
    }
    char part[96];
    std::snprintf(part, sizeof(part), "%s%s %s (%d)", s ? ", " : "", kShapeName[s],
                  ok ? "same" : "OFF", cases);
    summary += part;
    on += ok ? 1 : 0;
  }
  ctx.fpscr.restoreGuestBits(saved_csr);
  mem->SystemHeapFree(alloc);
  REXLOG_INFO("cod3: native FMOD resampler self-check ({:.1f} ms; versions and cases compared): {}; "
              "{} of {} now run as native code (cod3_native_resampler)",
              double(int64_t(NowNs()) - t0) / 1e6, summary, on, int(kShapeCount));
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_resampler_report, ReportCommand, "cod3/Performance",
                            "cod3_native_resampler_report: how many of FMOD's resampler calls ran "
                            "as native code, by version, written to the log");

// FMOD's linear resampler: native where Prepare() allows, the first calls of
// each version checked against the game's code, else the game's code.
REX_HOOK_RAW(sub_823BA518) {
  if (!REXCVAR_GET(cod3_native_resampler)) {
    __imp__sub_823BA518(ctx, base);
    return;
  }
  if (g_state.load(std::memory_order_acquire) != 2) {
    int expected = 0;
    if (g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> lock(g_lock);
      RunSelfCheck(ctx, base);
      g_state.store(2, std::memory_order_release);
    }
    if (g_state.load(std::memory_order_acquire) != 2) {
      __imp__sub_823BA518(ctx, base);  // another thread is checking
      return;
    }
  }
  static thread_local Call call;
  const Why why = Prepare(base, ctx.r3.u32, ctx.r4.s32, ctx.r5.u32, ctx.r6.s32, ctx.r7.u32,
                          ctx.r8.u32, ctx.r9.s32, &call);
  if (why != kOk || !g_ok[call.shape].load(std::memory_order_relaxed)) {
    g_why[why].fetch_add(1, std::memory_order_relaxed);
    g_guest.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_823BA518(ctx, base);
    return;
  }
  const uint64_t t0 = NowNs();
  Compute(base, ctx.fpscr, &call);
  {
    std::unique_lock<std::mutex> lock(g_lock);
    int& left = g_checks_left[call.shape];
    if (left > 0 || ++g_spot % kSpotEvery == 0) {
      __imp__sub_823BA518(ctx, base);  // the game's result stays; ctx ends as its code leaves it
      g_checked[call.shape].fetch_add(1, std::memory_order_relaxed);
      std::string diff;
      const bool guest_flush = (ctx.fpscr.csr & FlushMask()) == FlushMask();
      if (!SameAsGuest(base, call, &diff) || guest_flush != call.flush_end) {
        if (diff.empty()) diff = "the flush mode left";
        g_ok[call.shape].store(false, std::memory_order_relaxed);
        REXLOG_ERROR("cod3: native FMOD resampler: {} differs from the game's code in a call ({} "
                     "frames, {} channels; {}); it runs the game's code from now on",
                     kShapeName[call.shape], call.frames, call.channels, diff);
        return;
      }
      if (left > 0 && --left == 0) {
        REXLOG_INFO("cod3: native FMOD resampler ({}): {} calls the same as the game's code; "
                    "native from now on",
                    kShapeName[call.shape], kChecksPerShape);
      }
      MaybeReport();
      return;
    }
  }
  Commit(base, call);
  g_native_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  g_native_frames.fetch_add(uint64_t(call.frames), std::memory_order_relaxed);
  if ((g_native[call.shape].fetch_add(1, std::memory_order_relaxed) & 0x3FF) == 0) {
    std::lock_guard<std::mutex> lock(g_lock);
    MaybeReport();
  }
}
