// cod3 - [native-math] the C runtime's math and character functions as native
// code: sin, cos, atan, atan2, asin and acos, fmod, floor and ceil, _fpclass,
// _sptype and _copysign; tolower, toupper, isalpha, isdigit, isspace and
// isalnum; exp and log (setup tool 2.12.8); pow (setup tool 2.12.11); tan
// (setup tool 2.12.16).
//
// These are the Xbox 360 C runtime's own PowerPC functions, translated by the
// recompiler (call sites in the campaign: _fpclass 101, sin and cos about 90
// each, floor 44, ceil 27, tolower and toupper 16, asin 15, fmod 11, atan2 8,
// acos 5; about the same in cod3mp). Each one only reads its argument
// registers and a few of the game's read-only constants and returns one
// register, so a native version can be written out instruction by
// instruction:
//   - sin and cos (double): |x| times 1/pi (or (|x| + pi/2) times 1/pi for cos)
//     rounded to an integer n, the remainder r = |x| - n * pi in two fused
//     steps, an odd polynomial of degree 17 in r by fused multiply-adds, its
//     sign flipped for odd n; a fixed result (the game's constant) above a
//     limit; sin(+-0) is x itself and cos(0) the constant 1. The native code
//     makes the same operations in the same order, rounds the same way (the
//     current rounding mode, as the generated code does) and reads the same
//     constants from the game's memory, so every finite and infinite argument
//     gives the same bits. A NaN argument goes to the game's code: which NaN an
//     operation passes on when two operands are NaNs depends on the
//     instructions the compiler picked (see fmod_native.cpp).
//   - fmod (double): integer long division on the two values' bits (it never
//     touches the floating point unit), written out step by step with the
//     same 64-bit operations, including what it does at the edges (its NaN
//     constant for an infinite or NaN x, a NaN y or a zero y; x itself for an
//     infinite y; the shifts' behavior for counts of 64 and more).
//   - tolower / toupper: 'A'-'Z' (or 'a'-'z') tested on r3's low 32 bits, then
//     32 added to (subtracted from) the whole 64-bit register.
//   - isalpha: the halfword at index r3 (its low 32 bits, times 2, wrapping)
//     of the ctype table the game's locale data points to, and 0x103; the
//     table read must be plain guest memory (see crt_native.cpp). isdigit,
//     isspace and isalnum (setup tool 2.12.7) are the same with 4, 8 and
//     0x107 (isalnum's andi. sets cr0 as isalpha's does; the other two use
//     rlwinm, which doesn't).
//   - atan and atan2 (Cody-Waite: 1/|x| above 1, the 2-sqrt(3) reduction, a
//     rational polynomial, the quadrant's offset), asin and the asin/acos core
//     _asincos (sqrt((1-|x|)/2) above 0.5): the same operations in the same
//     order. Arguments whose arithmetic would make a NaN go to the game's code
//     (a NaN argument, asin or acos of more than 1, atan2 of two infinities),
//     so no NaN ever goes through the native arithmetic.
//   - floor and ceil (truncate, then -1 or +1; x itself above 1e18, for 0 and
//     for a NaN), _fpclass and _sptype (the class from the bits, as the code
//     computes it, high register halves included) and _copysign (the bits).
//   - exp (2.12.8): n = x / ln 2 rounded (fctid), r = x - n ln 2 in one fused
//     step, a rational polynomial in r, then 2^(n+1) put into the result's
//     exponent bits directly (no overflow check there); the limits pick +inf
//     or 0.0 last (fsel), so out-of-range arguments never depend on the
//     polynomial. This exp never sets errno. A NaN argument goes to the game's
//     code.
//   - log (2.12.8): 1.0 gives 0.0; +inf gives itself; a NaN, -inf or a
//     negative number the game's NaN constant negated; +-0 -inf; otherwise the
//     exponent and a mantissa in [0.5, 1) from the bits (denormals scaled up
//     first), the sqrt(0.5) split, a rational polynomial and the exponent
//     times ln 2 in two parts. No NaN goes through its arithmetic, so every
//     argument runs natively.
//   - pow (2.12.11; 78 call sites, 17 in the game's own code, the rest in
//     the libraries; float callers round its result): Cody and Waite's pow. y = 0 gives
//     1.0; x = 0 the signed zero or infinity y's sign and parity give; an
//     infinite x or y a constant (sub_8234DB98; its quirk: pow(+-1, -inf)
//     is the game's NaN); a negative x to a y that isn't an integer the NaN;
//     |y| above 1e20 +-inf, +-0 or +-1; a positive integer y up to 128 with
//     an integer x by squaring the mantissa; otherwise log2(x) in sixteenths
//     from the table of 2^(-j/16) (or from log() itself when x is within a
//     sixteenth of an octave of 1), y times it kept exact in pieces, and 2^w by a
//     polynomial times a table entry. Results outside 2^-1021 .. 2^1024 are
//     +-0 or +-inf (no denormals, no errno). Its helpers (the integer test
//     sub_8234DB28, frexp sub_82355B80, the exponent's get and set
//     sub_82355AA8 / sub_82355A80, the infinities sub_8234DB98) are written
//     out the same way inside it and not hooked (frexp and the exponent's set
//     have other callers in the C runtime, which keep the game's code). A NaN
//     x or y goes to the game's code: its last step is x + y.
//   - tan (2.12.16; 20 call sites in each program): Cody and Waite's tan. Its code
//     first sets round-to-nearest (_ctrlfp, which in this port can only see
//     and set the rounding mode) and puts the old mode back at the end, so the
//     native version runs only when the mode already is round-to-nearest (the
//     game's): n = x times 2/pi rounded to an integer (by adding and
//     subtracting 2^52, sub_82356730), r = x - n pi/2 in two fused steps, a
//     rational polynomial p(r)/q(r) (just r/1.0 below a small bound), and
//     -q/p for an odd n. +-0 gives x itself. A NaN or infinite x, |x| above
//     its limit (a total loss of precision) and a denormal r go to the game's
//     code: they end in _except1 or _handle_qnan1, which set errno.
//   Like the others, exp, log, pow and tan don't write the scratch words the
//   game's code leaves below the stack pointer (and log, pow and tan the
//   copies of x and y in their caller's argument area): only the result
//   register.
//
// Before the first native call, a check runs every original and every native
// version on thousands of arguments (the edges: zeros, denormals, infinities,
// the limits, multiples of pi/2 and the values a few steps from them, exact
// multiples for fmod with large exponent gaps; then random ones of every
// kind) and compares the returned register's bits. A function that differs
// once stays on the game's code for the session (logged). The first calls the
// game makes are compared too, and after them one call in kSpotEvery per
// thread. cod3_native_math (on) switches them all off at any time;
// cod3_native_math_check_all (off) compares every call (tests);
// cod3_native_math_test N runs N more random arguments per function on the
// next call; cod3_native_math_report writes the counts to the log.
//
// Both programs: cod3mp has the same functions (the same instructions; only
// the constants' addresses differ, COD3_ADDR) and tools/mp_aliases.py points
// tolower_guest, toupper_guest and the unnamed ones at its copies
// (mp/config/campaign_map.tsv).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
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

REXCVAR_DEFINE_BOOL(cod3_native_math, true, "cod3/Performance",
                    "The C runtime's math and character functions (sin, cos, atan, atan2, asin, "
                    "acos, tan, fmod, floor, ceil, exp, log, pow, _fpclass, tolower, toupper, "
                    "isalpha, isdigit, isspace, isalnum and helpers) run "
                    "as native code that does exactly what the game's own code does (checked at "
                    "start; off: the translated Xbox 360 code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_native_math_check_all, false, "cod3/Performance",
                    "Testing: every call of those functions runs both ways and the results are "
                    "compared (slower)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sin_guest);      // 0x8234B538 (cod3mp 0x8231AA78)
REX_EXTERN(__imp__cos_guest);      // 0x8234B610 (cod3mp 0x8231AB50)
REX_EXTERN(__imp__fmod_guest);     // 0x8234C850 (cod3mp 0x8231C5A0)
REX_EXTERN(__imp__tolower_guest);  // 0x8234BFC0 (cod3mp 0x8231BBF0)
REX_EXTERN(__imp__toupper_guest);  // 0x8234D410 (cod3mp 0x8231D160)
REX_EXTERN(__imp__isalpha_guest);  // 0x8234BFD8 (cod3mp 0x8231BD28)
REX_EXTERN(__imp__sub_8234BFF8);   // isdigit (cod3mp sub_8231BD48)
REX_EXTERN(__imp__sub_8234C038);   // isspace (cod3mp sub_8231BD88)
REX_EXTERN(__imp__sub_8234C058);   // isalnum (cod3mp sub_8231BDA8)
REX_EXTERN(__imp__sub_8234E468);   // _fpclass (cod3mp sub_8231DAC8)
REX_EXTERN(__imp__sub_82355AF8);   // _sptype (cod3mp sub_823256F8)
REX_EXTERN(__imp__sub_8234F0E0);   // floor (cod3mp sub_8231E620)
REX_EXTERN(__imp__sub_8234B740);   // ceil (cod3mp sub_8231AC80)
REX_EXTERN(__imp__sub_8234EC40);   // atan (cod3mp sub_8231E180)
REX_EXTERN(__imp__sub_8234ED28);   // atan2 (cod3mp sub_8231E268)
REX_EXTERN(__imp__sub_8234D010);   // asin (cod3mp sub_8231CD60)
REX_EXTERN(__imp__sub_8234CF18);   // _asincos, acos's core (cod3mp sub_8231CC68)
REX_EXTERN(__imp__sub_8234E430);   // _copysign (cod3mp sub_8231DA90)
REX_EXTERN(__imp__sub_8234F180);   // exp (cod3mp sub_823204E8)
REX_EXTERN(__imp__sub_8234E980);   // log (cod3mp sub_8231DEC0; log10 0x8234EB00 calls it)
REX_EXTERN(__imp__sub_8234DD18);   // pow (cod3mp sub_8231B278)
REX_EXTERN(__imp__sub_8234BBD8);   // tan (cod3mp sub_8231B808)

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

inline uint64_t LdD(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
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
inline double Dbl(uint64_t u) {
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}
inline uint64_t Bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}
inline double LdDv(const uint8_t* p) { return Dbl(LdD(p)); }  // lfd
inline double LdFs(const uint8_t* p) {                         // lfs: the float, widened
  const uint32_t w = LdW(p);
  float f;
  std::memcpy(&f, &w, 4);
  return double(f);
}

constexpr uint64_t kSign = 0x8000000000000000ull;

inline bool IsNanBits(uint64_t b) {
  return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0x000FFFFFFFFFFFFFull);
}

// ---------------------------------------------------------------------------
// The game's constants (read-only data of the program; cod3mp's sit elsewhere)
// ---------------------------------------------------------------------------

// sin's and cos's table: +0 pi/2, +8 1/pi, +16 the limit, +24 (float) cos's
// special argument, +28 / +32 (floats) the results' signs (+28 also cos's
// special result), +36 (float) 0.5, +40 / +48 pi in two parts, +56..+112 the
// polynomial's coefficients.
constexpr uint32_t kTrigTable = COD3_ADDR(0x825C67E0, 0x825CBD00);
constexpr uint32_t kTrigTableSize = 120;
constexpr uint32_t kOneAddr = COD3_ADDR(0x8207C9F8, 0x82085408);  // 1.0, the polynomial's last term
constexpr uint32_t kNanAddr = COD3_ADDR(0x825C6E58, 0x825CC368);  // the NaN sin, cos and fmod return
// isalpha reads the ctype table's address at +200 from here.
constexpr uint32_t kCtypeAddr = COD3_ADDR(0x825C6F68, 0x825CC478);
constexpr uint32_t kCtypePointer = kCtypeAddr + 200;
// atan's and atan2's table: +8 pi/2, +16 pi, +24 2-sqrt(3), +40 sqrt(3) - 1,
// +48 the limit above which atan is pi/2, +56..+112 the polynomial,
// +128 the four quadrant offsets, +168 (float) 0.0, +176 (float) 1.0.
constexpr uint32_t kAtanTable = COD3_ADDR(0x825C6910, 0x825CBE20);
constexpr uint32_t kAtanTableSize = 180;
// asin's and acos's: +0 (float) 0.0, +4 (float) 1.0, +8 (float) 2.0, +12
// (float) 0.5, +16..+40 the offsets (0, pi/4 and pi/2 pairs), +48..+120 the
// polynomial.
constexpr uint32_t kAsinTable = COD3_ADDR(0x825C6880, 0x825CBDA0);
constexpr uint32_t kAsinTableSize = 128;
constexpr uint32_t kRoundLimitAddr = COD3_ADDR(0x8205B148, 0x82059BD0);  // floor's and ceil's 1e18
constexpr uint32_t kZeroAddr = COD3_ADDR(0x8207C8D8, 0x82085410);        // 0.0 (_fpclass)
// exp's constants: +0 the overflow limit, +8 the underflow limit, +24 1/ln 2,
// +48..+88 the polynomial (+72 is also the 0.5 added last), +96 ln 2.
constexpr uint32_t kExpTable = COD3_ADDR(0x8205B3B8, 0x82059E58);
constexpr uint32_t kExpTableSize = 104;
// log's constants: +0 sqrt(0.5), +8 ln 2's second part, +40 and +64 the
// polynomial's first terms, +0x50..+0x70 the rest, +0x78 the denormal scale,
// +0x80 the smallest normal, +0x88 the largest finite.
constexpr uint32_t kLogTable = COD3_ADDR(0x8205B328, 0x82059DB8);
constexpr uint32_t kLogTableSize = 0x90;
constexpr uint32_t kInfAddr = COD3_ADDR(0x825C6E50, 0x825CC360);   // +inf (next to the NaN)
constexpr uint32_t kHalfAddr = COD3_ADDR(0x8207C838, 0x82085560);  // 0.5
// pow's (2.12.11): its table (Cody and Waite's): +8..+136 the 17 powers
// 2^(-j/16), +144..+208 their low parts, +216 1/ln 2 (log2 e), +224 its
// fraction, +232..+256 the log polynomial, +264..+312 the exp polynomial,
// +320 / +328 the overflow and underflow limits (times 16), +336 1/16.
constexpr uint32_t kPowTable = COD3_ADDR(0x8205B1D0, 0x82059BD8);
constexpr uint32_t kPowTableSize = 344;
constexpr uint32_t kMinusOneAddr = COD3_ADDR(0x8207CC58, 0x82085AA8);  // -1.0, the result's sign
constexpr uint32_t kPowHugeYAddr = COD3_ADDR(0x825C6900, 0x825CBD80);  // 1e20
constexpr uint32_t kPowIntYAddr = COD3_ADDR(0x82040098, 0x82059D30);   // 128.0
constexpr uint32_t kTwoAddr = COD3_ADDR(0x8207CD80, 0x820859F8);       // 2.0
constexpr uint32_t kSixteenAddr = COD3_ADDR(0x8204CEB0, 0x8204B870);   // 16.0
constexpr uint32_t kMinusZeroAddr = COD3_ADDR(0x825C6E70, 0x825CC380); // -0.0 (after the NaN)
// tan's (2.12.16): its table: +0 the factor x is scaled by (2/pi), +8 the
// remainder below which tan(r) is r, +16 the limit above which tan reports a
// total loss of precision, +32..+112 the rational polynomial (+48 also 1.0);
// pi/2 in two parts; its rounding helper's two constants (0.0 and 2^52).
// They are read where the game's code reads them (and their values logged
// once, by the check).
constexpr uint32_t kTanTable = COD3_ADDR(0x8205B150, 0x82059D38);
constexpr uint32_t kTanTableSize = 120;
constexpr uint32_t kTanHalfPiAddr = COD3_ADDR(0x825C6860, 0x825CBD88);  // 16 bytes
constexpr uint32_t kTanRoundAddr = COD3_ADDR(0x8205BF50, 0x8205A9E0);   // 16 bytes

// ---------------------------------------------------------------------------
// The functions, instruction by instruction
// ---------------------------------------------------------------------------

// fctid / fctidz as the generated code computes them (the current rounding
// mode for fctid, toward zero for fctidz; NaN gives the smallest integer, too
// large a value the largest).
inline int64_t Fctid(double v) {
  return std::isnan(v)                ? int64_t(0x8000000000000000ULL)
         : (v > double(LLONG_MAX))    ? LLONG_MAX
                                      : simde_mm_cvtsd_si64(simde_mm_load_sd(&v));
}
inline int64_t Fctidz(double v) {
  return std::isnan(v)                ? int64_t(0x8000000000000000ULL)
         : (v > double(LLONG_MAX))    ? LLONG_MAX
                                      : simde_mm_cvttsd_si64(simde_mm_load_sd(&v));
}
// fctiwz as the generated code computes it (the 32-bit result stfiwx stores).
inline uint32_t Fctiwz(double v) {
  const int64_t r = std::isnan(v)               ? int64_t(0x80000000U)
                    : (v >= double(INT_MAX))    ? INT_MAX
                                                : simde_mm_cvttsd_si32(simde_mm_load_sd(&v));
  return uint32_t(r);
}

// The polynomial both share: c112 r2^7 + ... + c56, then times r2 plus 1.
inline double Poly(const uint8_t* t, double r2, double one) {
  double p = std::fma(LdDv(t + 112), r2, LdDv(t + 104));
  p = std::fma(p, r2, LdDv(t + 96));
  p = std::fma(p, r2, LdDv(t + 88));
  p = std::fma(p, r2, LdDv(t + 80));
  p = std::fma(p, r2, LdDv(t + 72));
  p = std::fma(p, r2, LdDv(t + 64));
  p = std::fma(p, r2, LdDv(t + 56));
  return std::fma(p, r2, one);
}

// sin_guest (0x8234B538): f1 in, f1 out (bits). x must not be a NaN.
uint64_t NativeSin(uint8_t* base, uint64_t xb) {
  const uint8_t* t = Host(base, kTrigTable);
  const double x = Dbl(xb);
  const uint64_t ab = xb & ~kSign;                        // fabs f12,f1 (its bits kept at -8(r1))
  const double a = Dbl(ab);
  const double sign = x >= 0.0 ? LdFs(t + 28) : LdFs(t + 32);  // fsel f0,f1,f9,f0
  const double scaled = LdDv(t + 8) * a;                  // fmul f9,f9,f12
  const int64_t k = Fctid(scaled);                        // fctid f8,f9
  const double n = double(k);                             // fcfid f0,f8
  double r = -std::fma(LdDv(t + 40), n, -a);              // fnmsub f13,f13,f0,f12
  const int64_t q = Fctidz(n);                            // fctidz f8,f0 (its bit 0, below)
  r = -std::fma(LdDv(t + 48), n, -r);                     // fnmsub f13,f11,f0,f13
  const double r2 = r * r;                                // fmul f0,f13,f13
  double s = Poly(t, r2, LdDv(Host(base, kOneAddr)));     // fmadd f10.. / fmadd f0,f10,f0,f11
  s = s * r;                                              // fmul f0,f0,f13
  if (q & 1) s = Dbl(Bits(s) ^ kSign);                    // fneg f0,f0
  const double over = a - LdDv(t + 16);                   // fsub f13,f12,f13
  const double out = s * sign;                            // fmul f11,f0,f9
  const uint64_t res = over >= 0.0 ? LdD(Host(base, kNanAddr)) : Bits(out);  // fsel f0,f13,f0,f11
  return ab == 0 ? xb : res;                              // ld r10,-8(r1); cmpdi; beqlr
}

// cos_guest (0x8234B610): f1 in, f1 out (bits). x must not be a NaN.
uint64_t NativeCos(uint8_t* base, uint64_t xb) {
  const uint8_t* t = Host(base, kTrigTable);
  const uint64_t ab = xb & ~kSign;                        // fabs f12,f1
  const double a = Dbl(ab);
  const double h = LdDv(t + 0) + a;                       // fadd f11,f0,f12
  const double scaled = LdDv(t + 8) * h;                  // fmul f0,f0,f11
  const int64_t k = Fctid(scaled);                        // fctid f0,f0
  const double n = double(k);                             // fcfid f0,f0; fmr f7,f0
  const double m = n - LdFs(t + 36);                      // fsub f0,f0,f13
  const int64_t q = Fctidz(n);                            // fctidz f13,f7 (its bit 0, below)
  double r = -std::fma(LdDv(t + 40), m, -a);              // fnmsub f13,f10,f0,f12
  r = -std::fma(LdDv(t + 48), m, -r);                     // fnmsub f13,f9,f0,f13
  const double r2 = r * r;                                // fmul f0,f13,f13
  double s = Poly(t, r2, LdDv(Host(base, kOneAddr)));     // fmadd f9.. / fmadd f0,f9,f0,f10
  s = s * r;                                              // fmul f0,f0,f13
  if (q & 1) s = Dbl(Bits(s) ^ kSign);                    // fneg f0,f0
  const double over = h - LdDv(t + 16);                   // fsub f10,f11,f10
  const uint64_t res = over >= 0.0 ? LdD(Host(base, kNanAddr)) : Bits(s);  // fsel f1,f10,f13,f0
  if (a == LdFs(t + 24)) return Bits(LdFs(t + 28));       // fcmpu cr6,f12,f11; bnelr; lfs f1,28
  return res;
}

// The generated code's shifts: a count with bit 6 set gives 0, else the low 7
// bits (which are then below 64).
inline uint64_t Sld(uint64_t v, uint64_t count) {
  const uint8_t c = uint8_t(count);
  return (c & 0x40) ? 0 : (v << (c & 0x7F));
}
inline uint64_t Srd(uint64_t v, uint64_t count) {
  const uint8_t c = uint8_t(count);
  return (c & 0x40) ? 0 : (v >> (c & 0x7F));
}
inline uint64_t Cntlzd(uint64_t v) { return v == 0 ? 64 : uint64_t(__builtin_clzll(v)); }
inline uint64_t Rotl(uint64_t v, int n) { return (v << n) | (v >> (64 - n)); }  // n 1..63

// fmod_guest (0x8234C850): f1 = x, f2 = y in, f1 out (bits). Integer work only.
uint64_t NativeFmod(uint8_t* base, uint64_t x, uint64_t y) {
  const uint64_t nan_c = LdD(Host(base, kNanAddr));
  if (((x >> 48) & 0x7FF0) == 0x7FF0) return nan_c;      // lhz 16(r1): x infinite or NaN
  if (((y >> 48) & 0x7FF0) == 0x7FF0) {                  // lhz 24(r1): y infinite or NaN
    const uint32_t hi = uint32_t(y >> 32), lo = uint32_t(y);
    if (hi == 0x7FF00000u && lo == 0) return x;          // +inf
    if (hi != 0xFFF00000u) return nan_c;                 // loc_8234C8A0
    return lo == 0 ? x : nan_c;                          // -inf
  }
  // loc_8234C8B8
  const uint64_t ay = y & 0x7FFFFFFFFFFFFFFFull;          // clrldi r11,r10,1
  const uint64_t ax = x & 0x7FFFFFFFFFFFFFFFull;          // clrldi r9,r7,1
  if (ax < ay) return x;                                  // cmpld; bltlr
  const uint64_t sign = x & kSign;                        // rldicr r4,r7,0,0 (also at -16(r1))
  uint64_t ey = Rotl(y, 12) & 0x7FF;                      // rldicl r8,r10,12,53
  uint64_t my = Rotl(y, 11) & 0x7FFFFFFFFFFFFFFFull;      // rldicl r10,r10,11,1
  uint64_t ex = Rotl(x, 12) & 0x7FF;                      // rldicl r11,r7,12,53
  my &= 0xFFFFFFFFFFFFF800ull;                            // rldicr r10,r10,0,52
  const uint64_t top = kSign;                             // rldicr r6,r9,63,63
  uint64_t dy;                                            // r5
  if (int64_t(ey) == 0) {
    if (my == 0) return nan_c;                            // y is 0
    const uint64_t z = Cntlzd(my);
    ey = uint64_t(1) - z;                                 // subfic r8,r9,1
    dy = Sld(my, z);
  } else {
    dy = my | top;                                        // loc_8234C914
  }
  // loc_8234C918
  uint64_t mx = Rotl(x, 11) & 0x7FFFFFFFFFFFF800ull;      // rldimi r10,r7,11,1 into 0
  if (int64_t(ex) == 0) {
    if (mx == 0) return x;                                // beqlr
    const uint64_t z = Cntlzd(mx);
    mx = Sld(mx, z);
    ex = uint64_t(1) - z;                                 // subfic r11,r11,1
  } else {
    mx |= top;                                            // loc_8234C944
  }
  const uint64_t half = dy >> 1;                          // rldicl r7,r5,63,1
  while (int64_t(ex) > int64_t(ey)) {                     // loc_8234C974: cmpd; bgt
    const uint64_t d = mx >= dy ? dy : half;              // loc_8234C950
    mx -= d;
    const uint64_t z = Cntlzd(mx);
    ex -= z;
    mx = Sld(mx, z);
  }
  if (ex == ey && mx >= dy) {                             // bne / blt
    mx -= dy;
    const uint64_t z = Cntlzd(mx);
    ex -= z;
    mx = Sld(mx, z);
  }
  // loc_8234C99C
  if (mx == 0) return sign;                               // lfd f1,-16(r1)
  if (!(int64_t(ex) > 0)) {
    const uint64_t count = uint64_t(1) - ex;              // subfic r9,r11,1
    ex = 0;
    mx = Srd(mx, count);
  }
  // loc_8234C9B8
  const uint64_t e = Rotl(ex, 52) & 0xFFF0000000000000ull;  // rldicr r11,r11,52,11
  const uint64_t m = Rotl(mx, 53) & 0x000FFFFFFFFFFFFFull;  // rldicl r10,r10,53,12
  return m + e + sign;                                    // add; add
}

// tolower_guest (0x8234BFC0) / toupper_guest (0x8234D410): r3 in, r3 out.
inline uint64_t NativeTolower(uint64_t c) {
  return uint32_t(c - 65) > 25 ? c : c + 32;  // addi r11,r3,-65; cmplwi; bgtlr; addi r3,r3,32
}
inline uint64_t NativeToupper(uint64_t c) {
  return uint32_t(c - 97) > 25 ? c : c - 32;  // addi r11,r3,-97; cmplwi; bgtlr; addi r3,r3,-32
}

// isalpha_guest (0x8234BFD8): r3 in, r3 out; false when the table entry isn't
// plain guest memory (the game's code then reads it its way).
bool NativeIsalpha(uint8_t* base, uint64_t c, uint64_t* out) {
  const uint32_t index = uint32_t(c) << 1;                 // rlwinm r10,r3,1,0,30
  const uint32_t table = LdW(Host(base, kCtypePointer));   // lwz r11,200(r11)
  const uint32_t at = index + table;                       // lhzx r11,r10,r11
  if (!Plain(base, at, 2)) return false;
  *out = uint64_t(LdH(Host(base, at)) & 259);              // andi. r3,r11,259
  return true;
}

// isdigit (sub_8234BFF8), isspace (sub_8234C038) and isalnum (sub_8234C058):
// isalpha's code with another mask: rlwinm r3,r11,0,29,29 (4) and
// rlwinm r3,r11,0,28,28 (8), whose masks don't wrap, so the high half is 0;
// andi. r3,r11,263 (0x107).
bool NativeCtype(uint8_t* base, uint64_t c, uint32_t mask, uint64_t* out) {
  const uint32_t index = uint32_t(c) << 1;                 // rlwinm r10,r3,1,0,30
  const uint32_t table = LdW(Host(base, kCtypePointer));   // lwz r11,200(r11)
  const uint32_t at = index + table;                       // lhzx r11,r10,r11
  if (!Plain(base, at, 2)) return false;
  *out = uint64_t(LdH(Host(base, at)) & mask);
  return true;
}
constexpr uint32_t kDigitMask = 4, kSpaceMask = 8, kAlnumMask = 263;

// The generated code's rlwinm with a rotation of 0: the low word doubled into
// the high one, then masked (masks that wrap keep high bits).
inline uint64_t Rlw0(uint64_t v, uint64_t mask) {
  return (uint64_t(uint32_t(v)) | (v << 32)) & mask;
}

// _sptype (0x82355AF8): f1 in, r3 out: 1 +inf, 2 -inf, 3 quiet NaN,
// 4 signalling NaN, 0 anything else.
uint64_t NativeSptype(uint64_t xb) {
  const uint32_t hi = uint32_t(xb >> 32), lo = uint32_t(xb);  // lwz 16(r1); lwz 20(r1)
  if (hi == 0x7FF00000u) {
    if (lo == 0) return 1;
  } else if (hi == 0xFFF00000u && lo == 0) {                 // loc_82355B20
    return 2;
  }
  const uint32_t top = uint32_t(xb >> 48) & 0x7FF8;           // loc_82355B3C: lhz; rlwinm
  if (top == 32760) return 3;
  if (top == 32752 && ((hi & 0x7FFFF) != 0 || lo != 0)) return 4;  // loc_82355B54
  return 0;
}

// _fpclass (0x8234E468): f1 in, r3 out (the _FPCLASS_ bits, with the high
// half the code leaves).
uint64_t NativeFpclass(uint8_t* base, uint64_t xb) {
  const uint64_t half = (xb >> 48) & 0xFFFF;                  // lhz r11,112(r1)
  const uint64_t exp = Rlw0(half, 0x7FF0);                    // rlwinm r10,r11,0,17,27
  if (uint32_t(exp) == 32752) {                               // infinite or NaN
    const uint64_t t = NativeSptype(xb);                      // bl _sptype
    if (int32_t(t) == 1) return 512;
    if (int32_t(t) == 2) return 4;
    uint64_t r = uint64_t(int64_t(t) + -3);                   // addi r11,r3,-3
    r = uint32_t(r) == 0 ? 32 : uint64_t(__builtin_clz(uint32_t(r)));  // cntlzw
    r = Rotl(uint64_t(uint32_t(r)) | (r << 32), 27) & 0x1;    // rlwinm r11,r11,27,31,31
    return uint64_t(int64_t(r) + 1);
  }
  // loc_8234E4C0: the sign as all ones or zero (subfic; subfe)
  const uint64_t sign = Rlw0(half, 0x8000);
  const uint64_t ones = uint32_t(sign) == 0 ? 0 : ~uint64_t(0);
  if (uint32_t(exp) == 0 && ((uint32_t(xb >> 32) & 0xFFFFF) != 0 || uint32_t(xb) != 0)) {
    // loc_8234E4E4: a denormal
    const uint64_t r = Rlw0(Rlw0(ones, 0xFFFFFFF0), 0xFFFFFFFFFFFFFF9Full);
    return uint64_t(int64_t(r) + 128);
  }
  // loc_8234E4FC: zero or normal
  if (Dbl(xb) == LdDv(Host(base, kZeroAddr))) {               // fcmpu cr6,f1,f0
    return uint64_t(int64_t(Rlw0(ones, 0xFFFFFFE0)) + 64);
  }
  const uint64_t r = Rlw0(Rlw0(ones, 0xFFFFFFF8), 0xFFFFFFFFFFFFFF0Full);  // loc_8234E520
  return uint64_t(int64_t(r) + 256);
}

// floor (0x8234F0E0) and ceil (0x8234B740): f1 in, f1 out (bits). Every
// argument (a NaN comes back as itself, bit for bit, through the selects).
uint64_t NativeFloor(uint8_t* base, uint64_t xb) {
  const double x = Dbl(xb);
  const double t = double(Fctidz(x));                         // fctidz f0,f1; fcfid f0,f0
  const uint64_t ab = xb & ~kSign;                            // fabs f13,f1
  const double down = t - LdDv(Host(base, kOneAddr));         // fsub f11,f0,f12
  const double past = x - t;                                  // fsub f10,f1,f0
  const double room = LdDv(Host(base, kRoundLimitAddr)) - Dbl(ab);  // fsub f12,f12,f13
  const uint64_t nab = ab ^ kSign;                            // fneg f13,f13
  uint64_t r = past >= 0.0 ? Bits(t) : Bits(down);            // fsel f0,f10,f0,f11
  r = room >= 0.0 ? r : xb;                                   // fsel f0,f12,f0,f1
  return Dbl(nab) >= 0.0 ? xb : r;                            // fsel f1,f13,f1,f0
}
uint64_t NativeCeil(uint8_t* base, uint64_t xb) {
  const double x = Dbl(xb);
  const double t = double(Fctidz(x));                         // fctidz f0,f1; fcfid f0,f0
  const uint64_t ab = xb & ~kSign;                            // fabs f13,f1
  const double up = t + LdDv(Host(base, kOneAddr));           // fadd f11,f0,f12
  const double short_by = t - x;                              // fsub f10,f0,f1
  const double room = LdDv(Host(base, kRoundLimitAddr)) - Dbl(ab);  // fsub f12,f12,f13
  const uint64_t nab = ab ^ kSign;                            // fneg f13,f13
  uint64_t r = short_by >= 0.0 ? Bits(t) : Bits(up);          // fsel f0,f10,f0,f11
  r = room >= 0.0 ? r : xb;                                   // fsel f0,f12,f0,f1
  return Dbl(nab) >= 0.0 ? xb : r;                            // fsel f1,f13,f1,f0
}

// atan's and atan2's shared tail: the rational polynomial in r = f13, its
// sign flipped for the upper quadrants, plus the quadrant's offset.
inline double AtanTail(const uint8_t* t, double f13, int32_t quadrant) {
  const double r2 = f13 * f13;                                // fmul f0,f13,f13
  double f10 = LdDv(t + 112) + r2;                            // fadd f10,f11,f0
  double f11 = std::fma(LdDv(t + 80), r2, LdDv(t + 72));
  f10 = std::fma(f10, r2, LdDv(t + 104));
  f11 = std::fma(f11, r2, LdDv(t + 64));
  f10 = std::fma(f10, r2, LdDv(t + 96));
  f11 = std::fma(f11, r2, LdDv(t + 56));
  const double f12 = std::fma(f10, r2, LdDv(t + 88));
  double v = f11 * r2;                                        // fmul f0,f11,f0
  v = v * f13;                                                // fmul f0,f0,f13
  v = v / f12;                                                // fdiv f0,f0,f12
  v = v + f13;                                                // fadd f0,f0,f13
  if (quadrant > 1) v = Dbl(Bits(v) ^ kSign);                 // cmpwi r10,1; ble; fneg
  return LdDv(t + 128 + 8 * uint32_t(quadrant)) + v;          // lfdx f13; fadd f0,f13,f0
}

// atan (0x8234EC40): f1 in, f1 out (bits). x must not be a NaN.
uint64_t NativeAtan(uint8_t* base, uint64_t xb) {
  const uint8_t* t = Host(base, kAtanTable);
  const double x = Dbl(xb);
  const uint64_t ab = xb & ~kSign;                            // fabs f13,f1 (kept at -16(r1))
  double f13 = Dbl(ab);
  const double one = LdFs(t + 176);                           // lfs f0,176(r11)
  double res;
  if (f13 > one && f13 > LdDv(t + 48)) {
    res = LdDv(t + 8);                                        // lfd f0,8(r11): pi/2
  } else {
    int32_t quadrant = 0;
    if (f13 > one) {                                          // loc_8234EC70
      quadrant = 2;
      f13 = one / f13;
    }
    if (f13 > LdDv(t + 24)) {                                 // loc_8234EC80
      const double c40 = LdDv(t + 40);
      quadrant += 1;
      const double f11 = c40 + f13;
      const double f0 = std::fma(c40, f13, -one);             // fmsub f0,f12,f13,f0
      f13 = f0 / f11;
    }
    res = AtanTail(t, f13, quadrant);                         // loc_8234ECA0
  }
  // loc_8234ED0C
  const uint64_t out = x >= 0.0 ? Bits(res) : (Bits(res) ^ kSign);  // fneg f13; fsel f0,f1,f0,f13
  return ab == 0 ? xb : out;                                  // ld r11,-16(r1); cmpdi; beqlr
}

// atan2 (0x8234ED28): f1 = y, f2 = x in, f1 out (bits); false when the game's
// code must do it (a NaN, or both infinite: their quotient would be a NaN).
bool NativeAtan2(uint8_t* base, uint64_t yb, uint64_t xb, uint64_t* out) {
  if (IsNanBits(yb) || IsNanBits(xb)) return false;
  if ((yb & ~kSign) == 0x7FF0000000000000ull && (xb & ~kSign) == 0x7FF0000000000000ull)
    return false;
  const uint8_t* t = Host(base, kAtanTable);
  const double y = Dbl(yb), x = Dbl(xb);
  const double zero = LdFs(t + 168);                          // lfs f0,168(r11)
  double f0;
  if (x == zero) {                                            // fcmpu cr6,f2,f0
    if (y == zero) {
      if ((uint32_t(xb >> 32) & 0x80000000u) == 0) {          // lwz r10,24(r1); rlwinm.
        *out = yb;                                            // beqlr: y itself
      } else if ((uint32_t(yb >> 32) & 0x80000000u) == 0) {
        *out = LdD(t + 16);                                   // loc_8234ED70: pi
      } else {
        *out = LdD(t + 16) ^ kSign;                           // -pi
      }
      return true;
    }
    f0 = LdDv(t + 8);                                         // loc_8234ED78: pi/2
  } else {
    const double ax = Dbl(xb & ~kSign);                       // loc_8234ED80: fabs f12,f2
    int32_t quadrant = 0;
    double f13 = Dbl(yb & ~kSign);                            // fabs f13,f1
    double den = ax;                                          // fmr f0,f12
    if (f13 > den) {
      den = f13;
      quadrant = 2;
      f13 = ax;
    }
    f13 = f13 / den;                                          // loc_8234EDA4: fdiv f13,f13,f0
    if (f13 > LdDv(t + 24)) {
      const double c40 = LdDv(t + 40);
      quadrant += 1;
      const double one = LdFs(t + 176);
      const double f11 = c40 + f13;
      const double n = std::fma(c40, f13, -one);              // fmsub f0,f0,f13,f12
      f13 = n / f11;
    }
    f0 = AtanTail(t, f13, quadrant);                          // loc_8234EDCC
  }
  // loc_8234EE38
  const double back = LdDv(t + 16) - f0;                      // fsub f13,f13,f0
  uint64_t r = x >= 0.0 ? Bits(f0) : Bits(back);              // fsel f1,f2,f0,f13
  if ((uint32_t(yb >> 32) & 0x80000000u) != 0) r ^= kSign;    // lwz r11,16(r1); fneg f1,f1
  *out = r;
  return true;
}

// asin (0x8234D010) and _asincos (0x8234CF18, acos's core: r4 = 1 for acos,
// 0 for asin): f1 in, f1 out (bits); false when the game's code must do it (a
// NaN, |x| > 1: the square root's argument would be negative, or another r4).
// acos (0x8234D0D8) is "li r4,1; b _asincos", so it reaches the native code
// through _asincos.
bool NativeAsincos(uint8_t* base, uint64_t xb, uint64_t flag, bool inline_asin, uint64_t* out) {
  if (IsNanBits(xb)) return false;
  if (!inline_asin && flag > 1) return false;
  const uint8_t* t = Host(base, kAsinTable);
  const double x = Dbl(xb);
  double f13 = Dbl(xb & ~kSign);                              // fabs f13,f1
  double f0 = LdFs(t + 12);                                   // lfs f0,12(r11): 0.5
  uint64_t index;                                             // r10
  if (f13 > f0) {
    const double c4 = LdFs(t + 4);
    index = inline_asin ? 1 : uint64_t(1) - flag;             // li r10,1 / subfic r10,r4,1
    f13 = c4 - f13;                                           // fsub f13,f12,f13
    const double c8 = LdFs(t + 8);
    f0 = f13 * f0;                                            // fmul f0,f13,f0
    if (!(f0 >= 0.0)) return false;                           // fsqrt would make a NaN
    f13 = std::sqrt(f0);                                      // fsqrt f13,f0
    f13 = f13 * c8;                                           // fmul f13,f13,f11
    f13 = Dbl(Bits(f13) ^ kSign);                             // fneg f13,f13
  } else {
    index = inline_asin ? 0 : flag;                           // li r10,0 / mr r10,r4
    f0 = f13 * f13;                                           // fmul f0,f13,f13
    if ((inline_asin || int32_t(flag) == 0) && x == LdFs(t + 0)) {
      *out = xb;                                              // fcmpu cr6,f1,f12; beqlr
      return true;
    }
  }
  // loc_8234D060 / loc_8234CF70
  const uint32_t off = uint32_t(index << 3) & 0xFFFFFFF8u;    // rlwinm r10,r10,3,0,28
  double f10 = LdDv(t + 120) + f0;                            // fadd f10,f11,f0
  double f11 = std::fma(LdDv(t + 80), f0, LdDv(t + 72));
  f10 = std::fma(f10, f0, LdDv(t + 112));
  f11 = std::fma(f11, f0, LdDv(t + 64));
  f10 = std::fma(f10, f0, LdDv(t + 104));
  f11 = std::fma(f11, f0, LdDv(t + 56));
  f10 = std::fma(f10, f0, LdDv(t + 96));
  f11 = std::fma(f11, f0, LdDv(t + 48));
  const double f12 = std::fma(f10, f0, LdDv(t + 88));
  double v = f11 * f0;                                        // fmul f0,f11,f0
  v = v * f13;                                                // fmul f0,f0,f13
  v = v / f12;                                                // fdiv f0,f0,f12
  v = v + f13;                                                // fadd f0,f0,f13
  uint64_t pos, neg;
  if (inline_asin || int32_t(flag) == 0) {
    v = v + LdDv(t + 16 + off);                               // lfdx f9 / f13; fadd
    pos = Bits(v);
    neg = Bits(v) ^ kSign;                                    // fneg f13,f0
  } else {                                                    // loc_8234CFEC
    const double hi = LdDv(t + 32 + off) + v;                 // fadd f13,f13,f0
    const double lo = LdDv(t + 16 + off) - v;                 // fsub f0,f12,f0
    pos = Bits(lo);
    neg = Bits(hi);
  }
  *out = x >= 0.0 ? pos : neg;                                // fsel f1,f1,f0,f13
  return true;
}

// _copysign (0x8234E430): f1 = x, f2 = y in, f1 out: x's bits with y's sign.
inline uint64_t NativeCopysign(uint64_t xb, uint64_t yb) {
  const uint32_t hi = (uint32_t(xb >> 32) & 0x7FFFFFFFu) | (uint32_t(yb >> 32) & 0x80000000u);
  return (uint64_t(hi) << 32) | uint32_t(xb);                 // rlwimi r10,r11,0,1,31; stw; stw
}

// exp (sub_8234F180): f1 in, f1 out (bits). x must not be a NaN.
uint64_t NativeExp(uint8_t* base, uint64_t xb) {
  const uint8_t* t = Host(base, kExpTable);
  const double x = Dbl(xb);
  const double ln2 = LdDv(t + 96);                        // lfd f13,-19432(r10)
  const double scaled = x * LdDv(t + 24);                 // fmul f0,f1,f0
  const double n = double(Fctid(scaled));                 // fctid; fcfid
  const double r = -std::fma(n, ln2, -x);                 // fnmsub f13,f0,f13,f1
  const uint32_t k = Fctiwz(n);                           // fctiwz f10,f0; stfiwx; lwz r10
  const double g = r * r;                                 // fmul f0,f13,f13
  double p = std::fma(g, LdDv(t + 64), LdDv(t + 56));     // fmadd f10,f0,f12,f10
  double q = std::fma(g, LdDv(t + 88), LdDv(t + 80));     // fmadd f11,f0,f11,f12
  p = std::fma(p, g, LdDv(t + 48));                       // fmadd f10,f10,f0,f12
  const double half = LdDv(t + 72);
  q = std::fma(q, g, half);                               // fmadd f11,f11,f0,f12
  const double pr = p * r;                                // fmul f0,f10,f13
  const double den = q - pr;                              // fsub f13,f11,f0
  const uint64_t m = uint64_t(k) + 1;                     // addic. r10,r10,1 (64-bit)
  double y = pr / den;                                    // fdiv f0,f0,f13
  y = y + half;                                           // fadd f0,f0,f12
  if (uint32_t(m) != 0) {                                 // beq (cr0 from the low word)
    const uint64_t yb = Bits(y);
    const uint32_t hw = uint32_t(yb >> 48);               // lhz r9,-24(r1) / r8,-16(r1)
    const int64_t e = int64_t(int16_t(int64_t((hw >> 4) & 0x7FF) - 1022));  // rlwinm; addi; extsh
    const uint64_t sum = uint64_t(e) + m + 1022;          // add r10,r9,r10; addi r10,r10,1022
    const uint32_t top = (uint32_t(sum) << 4) & 0xFFFFFFF0u;  // rlwinm r10,r10,4,0,27
    const uint16_t nhw = uint16_t(top | (hw & 0x800Fu));  // andi. r8,r8,32783; or; sth
    y = Dbl((yb & 0x0000FFFFFFFFFFFFull) | (uint64_t(nhw) << 48));  // lfd f0,-32(r1)
  }
  const double over = x - LdDv(t + 0);                    // fsub f10,f1,f12
  const double under = LdDv(t + 8) - x;                   // fsub f11,f11,f1
  double res = over >= 0.0 ? LdDv(Host(base, kInfAddr)) : y;            // fsel f0,f10,f13,f0
  res = under >= 0.0 ? LdDv(Host(base, kZeroAddr)) : res;               // fsel f1,f11,f12,f0
  return Bits(res);
}

// log (sub_8234E980): f1 in, f1 out (bits). Every argument.
uint64_t NativeLog(uint8_t* base, uint64_t xb) {
  const uint8_t* t = Host(base, kLogTable);
  const double x = Dbl(xb);
  const double one = LdDv(Host(base, kOneAddr));          // lfd f8,-13832(r11)
  const double zero = LdDv(Host(base, kZeroAddr));
  if (x == one) return Bits(zero);                        // fcmpu cr6,f1,f8; lfd f1 (0.0)
  const uint32_t hw = uint32_t(xb >> 48);                 // stfd f1,16(r1); lhz r11,16(r1)
  const uint32_t ex = hw & 0x7FF0;                        // rlwinm r10,r11,0,17,27
  const uint64_t neg_nan = LdD(Host(base, kNanAddr)) ^ kSign;  // lfd f0 (the NaN); fneg f1,f0
  if (ex == 0x7FF0) {
    if (x > LdDv(t + 0x88)) return xb;                    // fcmpu; bgtlr: +inf itself
    return neg_nan;                                       // NaN, -inf
  }
  if (!(x > zero)) {
    if (x == zero) return LdD(Host(base, kInfAddr)) ^ kSign;  // +-0: -inf
    return neg_nan;                                       // negative
  }
  double v = x;
  uint32_t h = hw;
  int64_t e;
  if (x < LdDv(t + 0x80)) {                               // a denormal: scaled up first
    v = x * LdDv(t + 0x78);                               // fmul f1,f1,f0
    h = uint32_t(Bits(v) >> 48);                          // stfd; lhz
    e = int64_t((h >> 4) & 0x7FF) - 1075;                 // rlwinm r10,r11,28,21,31; addi
  } else {
    e = int64_t((ex >> 4) & 0xFFF) - 1022;                // rlwinm r10,r10,28,20,31; addi
  }
  const uint32_t nh = (h & 0x800Fu) | 0x3FE0u;            // andi.; ori r11,r11,16352
  const double mnt = Dbl((Bits(v) & 0x0000FFFFFFFFFFFFull) | (uint64_t(nh & 0xFFFF) << 48));  // sth; lfd
  const double half = LdDv(Host(base, kHalfAddr));        // lfd f13,-14280(r9)
  double f0, f13;
  if (mnt > LdDv(t + 0)) {                                // fcmpu cr6,f0,f13 (sqrt(0.5))
    const double f12 = mnt - half;                        // fsub f12,f0,f13
    const double f11 = mnt + one;                         // fadd f11,f0,f8
    f0 = f12 - half;                                      // fsub f0,f12,f13
    f13 = f11 * half;                                     // fmul f13,f11,f13
  } else {
    f0 = mnt - half;                                      // fsub f0,f0,f13
    e -= 1;                                               // addi r10,r10,-1
    const double f12 = f0 + one;                          // fadd f12,f0,f8
    f13 = f12 * half;                                     // fmul f13,f12,f13
  }
  const double nd = double(int64_t(int32_t(e)));          // extsw r9,r10; std; lfd; fcfid
  const double z = f0 / f13;                              // fdiv f13,f0,f13
  const double w = z * z;                                 // fmul f0,f13,f13
  const double f7a = w - LdDv(t + 0x68);                  // fsub f7,f0,f9
  const double f6 = nd * LdDv(t + 0x60);                  // fmul f6,f12,f9
  double f9 = -std::fma(w, LdDv(t + 0x70), -LdDv(t + 40));  // fnmsub f9,f0,f11,f9
  const double f7 = std::fma(f7a, w, LdDv(t + 64));       // fmadd f7,f7,f0,f11
  f9 = std::fma(f9, w, -LdDv(t + 0x58));                  // fmsub f9,f9,f0,f11
  const double f11 = std::fma(f7, w, -LdDv(t + 0x50));    // fmsub f11,f7,f0,f11
  double r = f9 * w;                                      // fmul f0,f9,f0
  r = r / f11;                                            // fdiv f0,f0,f11
  r = r + one;                                            // fadd f0,f0,f8
  r = std::fma(r, z, -f6);                                // fmsub f0,f0,f13,f6
  r = std::fma(nd, LdDv(t + 8), r);                       // fmadd f1,f12,f10,f0
  return Bits(r);
}

// ---------------------------------------------------------------------------
// pow (setup tool 2.12.11) and the helpers it calls (written out inside it)
// ---------------------------------------------------------------------------

// sub_8234DB28 (cod3mp sub_8231B088): 0 when v is not an integer, 2 when it is
// an even one, 1 an odd one, as fctid in the current rounding mode sees it:
// 2^63 and more, the infinities and the denormals count as not integers, +-0
// as even.
int PowIntType(uint8_t* base, uint64_t vb) {
  const double v = Dbl(vb);
  if ((uint32_t(vb >> 48) & 0x7FF0) == 0) {               // lhz; rlwinm. r11,r11,0,17,27
    if ((uint32_t(vb >> 32) & 0xFFFFF) != 0) return 0;    // clrlwi. r11,r11,12
    if (uint32_t(vb) != 0) return 0;                      // lwz r11,20(r1); cmplwi
  }
  if (double(Fctid(v)) != v) return 0;                    // fctid; fcfid; fcmpu; bne
  const double h = v * LdDv(Host(base, kHalfAddr));       // fmul f0,f1,f0 (0.5)
  return double(Fctid(h)) == h ? 2 : 1;
}

// sub_82355B80 (cod3mp sub_82325780): frexp: v's mantissa in [0.5, 1) with
// v's sign (0 itself for +-0), and its exponent in *exp.
uint64_t PowFrexp(uint8_t* base, uint64_t vb, int32_t* exp) {
  const double zero = LdDv(Host(base, kZeroAddr));
  if (Dbl(vb) == zero) {                                  // fcmpu cr6,f1,f0; li r9,0
    *exp = 0;
    return vb;
  }
  const uint32_t hw = uint32_t(vb >> 48);                 // lhz r8 / mr r7,r8
  uint64_t f0;                                            // what -8(r1) holds
  int32_t e;
  if ((hw & 0x7FF0) == 0 && ((uint32_t(vb >> 32) & 0xFFFFF) != 0 || uint32_t(vb) != 0)) {
    // A denormal: the mantissa shifted up until its top bit is where the
    // implicit bit goes (the sign shifted out, put back after).
    e = -1021;                                            // li r9,-1021
    const bool neg = Dbl(vb) < zero;                      // r5
    uint32_t hi = uint32_t(vb >> 32), lo = uint32_t(vb);  // r6, r10
    uint32_t h8 = hw;                                     // r8
    if ((hw & 0x10) == 0) {                               // rlwinm. r11,r7,0,27,27
      do {
        hi <<= 1;                                         // rlwinm r6,r6,1,0,30
        if (lo & 0x80000000u) hi |= 1;                    // rlwinm. r11,r10,0,0,0; ori
        h8 = hi >> 16;                                    // stw r6; lhz r8,16(r1)
        lo <<= 1;                                         // rlwinm r10,r10,1,0,30
        e -= 1;                                           // addi r9,r9,-1
      } while ((h8 & 0x10) == 0);
    }
    uint32_t top = h8 & 0xFFEFu;                          // andi. r11,r8,65519; sth
    if (neg) top |= 0x8000u;                              // ori r11,r11,32768; sth
    f0 = (uint64_t(top & 0xFFFF) << 48) | (uint64_t(hi & 0xFFFF) << 32) | lo;  // lfd f0,16(r1)
  } else {
    e = int32_t((hw & 0x7FF0) >> 4) - 1022;               // rlwinm r11,r11,28,20,31; addi
    f0 = vb;                                              // stfd f1,-8(r1)
  }
  const uint32_t nh = (uint32_t(f0 >> 48) & 0x800Fu) | 0x3FE0u;  // lhz; andi.; ori; sth
  *exp = e;
  return (f0 & 0x0000FFFFFFFFFFFFull) | (uint64_t(nh) << 48);    // lfd f1,-16(r1)
}

// sub_82355AA8 (cod3mp sub_823256A8): the exponent field minus 1022.
inline int32_t PowGetExp(uint64_t vb) {
  return int32_t(int16_t(int32_t((uint32_t(vb >> 48) >> 4) & 0x7FF) - 1022));  // rlwinm; addi; extsh
}

// sub_82355A80 (cod3mp sub_82325680): v with its exponent field set to e + 1022.
inline uint64_t PowSetExp(uint64_t vb, int32_t e) {
  const uint32_t top = (uint32_t(e + 1022) << 4) | (uint32_t(vb >> 48) & 0x800Fu);  // rlwinm; andi.; or
  return (vb & 0x0000FFFFFFFFFFFFull) | (uint64_t(top & 0xFFFF) << 48);              // sth; lfd
}

// sub_8234DB98 (cod3mp sub_8231B0F8): pow when x or y is infinite (neither a
// NaN): constants only. Its quirk: pow(+-1, -inf) is the game's NaN.
uint64_t PowInfinite(uint8_t* base, uint64_t xb, uint64_t yb) {
  const uint64_t inf = LdD(Host(base, kInfAddr)), zero = LdD(Host(base, kZeroAddr));
  const uint64_t one = LdD(Host(base, kOneAddr));
  const double ax = Dbl(xb & ~kSign), y = Dbl(yb);        // fabs f0,f1
  const uint32_t yhi = uint32_t(yb >> 32), xhi = uint32_t(xb >> 32);
  if (yhi == 0x7FF00000u && uint32_t(yb) == 0) {          // y = +inf
    if (ax > Dbl(one)) return inf;
    if (ax < Dbl(one)) return zero;
    return one;                                           // stfd f13,0(r31)
  }
  if (yhi == 0xFFF00000u && uint32_t(yb) == 0) {          // y = -inf
    if (ax > Dbl(one)) return zero;
    if (ax < Dbl(one)) return inf;
    return LdD(Host(base, kNanAddr));                     // li r30,1; the NaN
  }
  if (xhi == 0x7FF00000u) {                               // x = +inf (y finite)
    if (y > Dbl(zero)) return inf;
    if (y < Dbl(zero)) return zero;
    return one;
  }
  // x = -inf (y finite): the sign from y's parity.
  const int t = PowIntType(base, yb);
  if (y > Dbl(zero)) return t == 1 ? (inf ^ kSign) : inf;           // fneg f0,f0
  if (!(y < Dbl(zero))) return one;
  return t == 1 ? LdD(Host(base, kMinusZeroAddr)) : zero;
}

// The end both of pow's ways share (0x8234E160): v times 2^e with the
// result's sign, +-inf above 2^1024 and +-0 below 2^-1021 (no denormals).
inline uint64_t PowScale(uint8_t* base, double v, int32_t e, double sign) {
  if (e > 1024) return Bits(LdDv(Host(base, kInfAddr)) * sign);     // fmul f1,f0,f25
  if (e < -1021) return Bits(sign * LdDv(Host(base, kZeroAddr)));   // fmul f1,f25,f27
  return Bits(Dbl(PowSetExp(Bits(v), e)) * sign);                    // fmul f1,f1,f25
}

// pow (sub_8234DD18, cod3mp sub_8231B278): f1 = x, f2 = y in, f1 out (bits).
// Neither may be a NaN. use_log: whether the native log may stand in for the
// game's (its check passed); false when this argument needs it and it may not.
bool NativePow(uint8_t* base, uint64_t xb, uint64_t yb, bool use_log, uint64_t* out) {
  const double zero = LdDv(Host(base, kZeroAddr));        // f27
  const double one = LdDv(Host(base, kOneAddr));          // f26
  const double y = Dbl(yb);                               // f30
  double x = Dbl(xb);                                     // f28
  if (y == zero) {                                        // fcmpu cr6,f30,f27; bne
    *out = LdD(Host(base, kOneAddr));
    return true;
  }
  if (x == zero) {
    const int t = PowIntType(base, yb);
    if (y < zero) {
      const uint64_t inf = LdD(Host(base, kInfAddr));
      *out = t == 1 ? NativeCopysign(inf, xb) : inf;     // bl _copysign (x's sign)
      return true;
    }
    if (y > zero) {
      *out = t == 1 ? xb : Bits(zero);                    // fmr f1,f28 / fmr f1,f27
      return true;
    }
  }
  const uint32_t hx = uint32_t(xb >> 48), hy = uint32_t(yb >> 48);
  if ((hx & 0x7FF0) == 0x7FF0 || (hy & 0x7FF0) == 0x7FF0) {
    *out = PowInfinite(base, xb, yb);                     // 0x8234E180: no NaN, so an infinity
    return true;
  }
  double sign = one;                                      // f25
  if (x < zero) {
    const int t = PowIntType(base, yb);
    if (t == 0) {                                         // a negative number to a fraction
      *out = LdD(Host(base, kNanAddr));
      return true;
    }
    if (t == 1) sign = LdDv(Host(base, kMinusOneAddr));
    x = Dbl(Bits(x) ^ kSign);                             // fneg f28,f28
  }
  const uint64_t inf = LdD(Host(base, kInfAddr));
  if (Dbl(yb & ~kSign) > LdDv(Host(base, kPowHugeYAddr))) {  // |y| > 1e20
    if (y < zero) x = one / x;                            // fdiv f28,f26,f28
    if (x > one) *out = Bits(Dbl(inf) * sign);
    else if (x < one) *out = Bits(sign * zero);
    else *out = Bits(sign);
    return true;
  }
  int32_t m = 0;
  double g = Dbl(PowFrexp(base, Bits(x), &m));            // f29; m at 80(r1)
  if (!(y > LdDv(Host(base, kPowIntYAddr))) && PowIntType(base, Bits(x)) != 0 &&
      PowIntType(base, yb) != 0 && y > zero) {
    // A positive integer y up to 128 and an integer x: by squaring.
    const uint32_t n = Fctiwz(y);                         // fctiwz f0,f30; stfiwx; lwz r11
    const int64_t nm = int64_t(int32_t(n)) * int64_t(m);  // mullw r31,r11,r10
    double r = one;                                       // f31
    for (int32_t k = int32_t(n); k != 0; k >>= 1) {       // srawi. r11,r11,1
      if (k & 1) r = r * g;                               // fmul f31,f31,f29
      g = g * g;                                          // fmul f29,f29,f29
    }
    const int32_t e = int32_t(int64_t(PowGetExp(Bits(r))) + nm);  // add r4,r3,r31 (cmpwi: low word)
    if (e > 2560) *out = Bits((Dbl(inf) * r) * sign);
    else if (e < -2557) *out = Bits((r * sign) * zero);
    else *out = PowScale(base, r, e, sign);
    return true;
  }
  // The general way (0x8234DF24): x = 2^m g, g = 2^(-p/16) (1 + small).
  const uint8_t* t = Host(base, kPowTable);
  uint32_t p = 1;                                         // li r11,1
  if (!(g > LdDv(t + 72))) p = 9;
  if (!(g > LdDv(t + 32 + p * 8))) p += 4;                // lfdx f0,r10,r9 (r31 + 32)
  if (!(g > LdDv(t + 16 + p * 8))) p += 2;                // (r31 + 16)
  const uint32_t m16 = uint32_t(m) << 4;                  // rlwinm r10,r10,4,0,27
  const int32_t q = int32_t(m16 - p);                     // subf. r9,r11,r10 (cr0: the low word)
  const int32_t dist = q < 0 ? int32_t(p - m16) : q;      // subf r10,r10,r11 / mr r10,r9
  const double sixteenth = LdDv(t + 336);                 // f31
  double u1, u2;                                          // f10, f13: log2(x) in two parts
  if (dist == 1) {                                        // cmpwi cr6,r10,1
    if (!use_log) return false;
    u1 = Dbl(NativeLog(base, Bits(x))) * LdDv(t + 216);   // bl log; fmul f10,f1,f0
    u2 = zero;                                            // fmr f13,f27
  } else {
    const double a1 = LdDv(t + 8 + p * 8);                // lfdx f0,r10,r8 (r31 + 8)
    const uint32_t half = (p + 1) >> 1;                   // addi; srawi; addze (p + 1 > 0)
    const double d = g - a1;                              // fsub f10,f29,f0
    const double s0 = a1 + g;                             // fadd f0,f0,f29
    const double lo = LdDv(t + 144 + half * 8);           // lfdx f13,r11,r7 (r31 + 144)
    const double fq = double(int64_t(q));                 // extsw; std; lfd; fcfid
    double s = d - lo;                                    // fsub f13,f10,f13
    u1 = fq * sixteenth;                                  // fmul f10,f9,f31
    s = s / s0;                                           // fdiv f13,f13,f0
    const double z = s * LdDv(Host(base, kTwoAddr));      // fmul f0,f13,f0
    const double v = z * z;                               // fmul f13,f0,f0
    const double zf = z * LdDv(t + 224);                  // fmul f8,f0,f9
    double pl = std::fma(v, LdDv(t + 256), LdDv(t + 248));  // fmadd f9,f13,f12,f9
    pl = std::fma(pl, v, LdDv(t + 240));                  // fmadd f9,f9,f13,f12
    pl = std::fma(pl, v, LdDv(t + 232));                  // fmadd f12,f9,f13,f12
    double w = pl * v;                                    // fmul f13,f12,f13
    w = w * z;                                            // fmul f13,f13,f0
    w = std::fma(w, LdDv(t + 216), zf);                   // fmadd f13,f13,f11,f8
    u2 = w + z;                                           // fadd f13,f13,f0
  }
  // y log2(x) in sixteenths (0x8234E040), kept exact in pieces.
  const double c16 = LdDv(Host(base, kSixteenAddr));      // f0
  double f11 = u2 * y;                                    // fmul f11,f13,f30
  double f12 = y * c16;                                   // fmul f12,f30,f0
  double f13 = double(Fctid(f12));                        // fctid; fcfid
  f13 = f13 * sixteenth;                                  // y1
  f12 = y - f13;                                          // y2
  f12 = std::fma(f12, u1, f11);                           // fmadd f12,f12,f10,f11
  f11 = f12 * c16;
  f11 = double(Fctid(f11));
  f11 = f11 * sixteenth;
  f13 = std::fma(f13, u1, f11);                           // fmadd f13,f13,f10,f11
  const double f9 = f12 - f11;                            // fsub f9,f12,f11
  f12 = f13 * c16;
  f12 = double(Fctid(f12));
  f12 = f12 * sixteenth;
  f13 = f13 - f12;
  f13 = f13 + f9;
  f11 = f13 * c16;
  f11 = double(Fctid(f11));
  f11 = f11 * sixteenth;
  f12 = f12 + f11;                                        // w1
  f13 = f13 - f11;                                        // w2
  const double w16 = f12 * c16;                           // fmul f0,f12,f0
  if (w16 > LdDv(t + 320)) {                              // bgt 0x8234de40
    *out = Bits(Dbl(inf) * sign);
    return true;
  }
  if (w16 < LdDv(t + 328)) {                              // blt 0x8234de58
    *out = Bits(sign * zero);
    return true;
  }
  uint32_t iw = Fctiwz(w16);                              // fctiwz; stfiwx; lwz r11
  if (f13 > zero) {                                       // fcmpu cr6,f13,f27; ble
    iw += 1;                                              // addi r11,r11,1
    f13 = f13 - sixteenth;                                // fsub f13,f13,f31
  }
  double pe = std::fma(f13, LdDv(t + 312), LdDv(t + 304));  // fmadd f12,f13,f0,f12
  pe = std::fma(pe, f13, LdDv(t + 296));
  pe = std::fma(pe, f13, LdDv(t + 288));
  pe = std::fma(pe, f13, LdDv(t + 280));
  pe = std::fma(pe, f13, LdDv(t + 272));
  pe = std::fma(pe, f13, LdDv(t + 264));                  // fmadd f0,f12,f13,f0
  const int32_t iws = int32_t(iw);
  int32_t mp = iws >> 4;                                  // srawi r9,r11,4
  if (iws < 0 && (iw & 0xF) != 0) mp += 1;                // addze r9,r9 (rounds toward zero)
  mp += iws >= 0 ? 1 : 0;                                 // cntlzw; cntlzw; rlwinm; xori; add r30
  const uint32_t pp = (uint32_t(mp) << 4) - iw;           // rlwinm r9,r30,4,0,27; subf r11,r11,r9
  if (pp > 16) return false;                              // (0..16 always: the table's 17 powers)
  const double a = LdDv(t + 8 + pp * 8);                  // lfdx f12,r11,r10 (r31 + 8)
  pe = std::fma(pe, f13, one);                            // fmadd f0,f0,f13,f26
  const double r = pe * a;                                // fmul f31,f0,f12
  const int32_t e = PowGetExp(Bits(r)) + mp;              // add r4,r3,r30
  *out = PowScale(base, r, e, sign);
  return true;
}

// ---------------------------------------------------------------------------
// tan (setup tool 2.12.16) and the rounding helper it calls (written out
// inside it)
// ---------------------------------------------------------------------------

// sub_82356730 (cod3mp sub_82326330): v rounded to an integer in the current
// rounding mode by adding and subtracting its constant (2^52): v itself when
// it equals its other constant (0.0) or when |v| isn't below the first.
inline double TanRound(uint8_t* base, double v) {
  const uint8_t* k = Host(base, kTanRoundAddr);
  const double zero = LdDv(k + 0), big = LdDv(k + 8);  // lfd f4,-16560(r12); lfd f5,-16552(r12)
  if (v == zero) return v;                                 // fcmpu cr0,f1,f4; beq-
  if (!(Dbl(Bits(v) & ~kSign) < big)) return v;            // fabs f6,f1; fcmpu cr0,f6,f5; bge-
  if (v < zero) return (v - big) + big;                    // fsub f4,f1,f5; fadd f1,f4,f5
  return (v + big) - big;                                  // fadd f4,f1,f5; fsub f1,f4,f5
}

// tan (sub_8234BBD8, cod3mp sub_8231B808): f1 in, f1 out (bits); false when
// the game's code must do it: a NaN or an infinity (_sptype, then
// _handle_qnan1 or _except1), |x| above its limit (_except1: a total loss of
// precision), or a remainder r that is a denormal (_except1: underflow).
// Round-to-nearest must be the current rounding mode (the hook checks it the
// way tan's code reads it): tan's code sets that mode for its arithmetic
// (_ctrlfp, sub_82356780) and puts the old one back at the end, and in this
// port the mode is all of the control word the code can see.
bool NativeTan(uint8_t* base, uint64_t xb, uint64_t* out) {
  if (((xb >> 48) & 0x7FF0) == 0x7FF0) return false;      // lhz r11,160(r1); rlwinm; cmplwi 32752
  const double x = Dbl(xb);
  const double zero = LdDv(Host(base, kZeroAddr));         // lfd f30,-14120(r11)
  if (x == zero) {                                         // fcmpu cr6,f31,f30; bne cr6
    *out = xb;                                             // fmr f1,f31 (after the restore)
    return true;
  }
  const uint8_t* t = Host(base, kTanTable);
  const double ax = x < zero ? Dbl(xb ^ kSign) : x;        // fneg f0,f31 / fmr f0,f31
  if (ax > LdDv(t + 16)) return false;                     // lfd f13,16(r31); fcmpu; ble cr6
  const double n = TanRound(base, x * LdDv(t + 0));        // fmul f1,f31,f0; bl sub_82356730
  const uint8_t* h = Host(base, kTanHalfPiAddr);
  const double f0 = -std::fma(LdDv(h + 0), n, -x);         // fnmsub f0,f0,f1,f31
  const double r = -std::fma(LdDv(h + 8), n, -f0);         // fnmsub f1,f13,f1,f0 (stfd 88(r1))
  const uint32_t q = uint32_t(Fctidz(Dbl(Bits(n) & ~kSign)));  // fabs f12,f1; fctidz; stfiwx 80(r1)
  const double ar = r < zero ? Dbl(Bits(r) ^ kSign) : r;   // fneg f0,f1 / fmr f0,f1
  double num, den;                                         // f12 and f0
  if (ar < LdDv(t + 8)) {                                  // lfd f13,8(r31); fcmpu; bge cr6
    const uint64_t rb = Bits(r);                           // lhz / lwz / lwz of 88(r1)
    if (((rb >> 48) & 0x7FF0) == 0 && (rb & 0x000FFFFFFFFFFFFFull) != 0)
      return false;                                        // a denormal: _except1(18, ...)
    num = r;                                               // fmr f12,f1
    den = LdDv(t + 48);                                    // lfd f0,48(r31)
  } else {
    const double r2 = r * r;                               // fmul f0,f1,f1
    double p = -std::fma(r2, LdDv(t + 112), -LdDv(t + 32));  // fnmsub f11,f0,f13,f11
    double s = std::fma(r2, LdDv(t + 80), -LdDv(t + 104));   // fmsub f12,f0,f12,f13
    p = std::fma(p, r2, -LdDv(t + 96));                    // fmsub f11,f11,f0,f13
    s = std::fma(s, r2, LdDv(t + 64));                     // fmadd f10,f12,f0,f13
    p = std::fma(p, r2, LdDv(t + 48));                     // fmadd f11,f11,f0,f13
    s = std::fma(s, r2, -LdDv(t + 88));                    // fmsub f10,f10,f0,f12
    num = p * r;                                           // fmul f12,f11,f1
    den = std::fma(s, r2, LdDv(t + 48));                   // fmadd f0,f10,f0,f13
  }
  // n odd: -1/tan(r); even: tan(r). Then rlwinm. r11,r30,0,28,28: the old
  // control word's inexact-exception bit, which _ctrlfp hands back inverted
  // (xori 248), so it is always set here (the word's other bits read as 0)
  // and tan's code restores the mode and returns, never _except1(16, ...).
  const double res = (q & 1) ? den / Dbl(Bits(num) ^ kSign)  // fneg f13,f12; fdiv f2,f0,f13
                             : num / den;                    // fdiv f2,f12,f0
  *out = Bits(res);
  return true;
}

// Whether the host's rounding mode is round-to-nearest, as tan's mffs (the
// generated code's loadFromHost) reads it.
bool RoundingNearest() {
  rex::ppc::FPSCRRegister probe{};
  return probe.loadFromHost() == rex::ppc::kRoundNearest;
}

// ---------------------------------------------------------------------------
// Which function, counters
// ---------------------------------------------------------------------------

enum Fn : int {
  kSin = 0, kCos, kFmod, kTolower, kToupper, kIsalpha,
  kFpclass, kSptype, kFloor, kCeil, kAtan, kAtan2, kAsin, kAsincos, kCopysign,
  kIsdigit, kIsspace, kIsalnum,  // 2.12.7 (after the others: their checks' random numbers stay)
  kExp, kLog,                    // 2.12.8
  kPow,                          // 2.12.11
  kTan,                          // 2.12.16
  kFnCount
};
constexpr const char* kFnName[kFnCount] = {"sin",   "cos",     "fmod",     "tolower", "toupper",
                                           "isalpha", "_fpclass", "_sptype", "floor",  "ceil",
                                           "atan",  "atan2",   "asin",     "_asincos", "_copysign",
                                           "isdigit", "isspace", "isalnum", "exp",   "log",
                                           "pow",   "tan"};

// Which functions run natively (set by the start-up check; a difference found
// later turns one off).
std::atomic<bool> g_fn_ok[kFnCount]{};

struct Counters {
  std::atomic<uint64_t> native[kFnCount]{};
  std::atomic<uint64_t> guest[kFnCount]{};    // left to the game's code (NaN, memory)
  std::atomic<uint64_t> checked[kFnCount]{};  // run both ways
  uint32_t tick[kFnCount] = {};               // this thread only
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
inline void Bump(std::atomic<uint64_t>& c) {
  c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kFirstReportNs = int64_t(60) * 1000000000;   // a minute after the check
constexpr int64_t kReportEveryNs = int64_t(600) * 1000000000;  // then every 10 minutes
std::atomic<int64_t> g_next_report_ns{0};                      // 0: none planned
std::atomic<bool> g_differed[kFnCount]{};

void Report() {
  uint64_t native[kFnCount] = {}, guest[kFnCount] = {}, checked[kFnCount] = {};
  {
    std::lock_guard lock(g_counters_mutex);
    for (const Counters* c : g_counters) {
      for (int f = 0; f < kFnCount; ++f) {
        native[f] += c->native[f].load(std::memory_order_relaxed);
        guest[f] += c->guest[f].load(std::memory_order_relaxed);
        checked[f] += c->checked[f].load(std::memory_order_relaxed);
      }
    }
  }
  std::string line, left, differed;
  for (int f = 0; f < kFnCount; ++f) {
    char part[160];
    std::snprintf(part, sizeof(part), "%s%s %llu (%llu compared)", f ? ", " : "", kFnName[f],
                  static_cast<unsigned long long>(native[f]),
                  static_cast<unsigned long long>(checked[f]));
    line += part;
    if (guest[f]) {
      std::snprintf(part, sizeof(part), "%s%s %llu", left.empty() ? "" : ", ", kFnName[f],
                    static_cast<unsigned long long>(guest[f]));
      left += part;
    }
    if (g_differed[f].load(std::memory_order_relaxed)) {
      differed += std::string(differed.empty() ? "" : ", ") + kFnName[f];
    }
  }
  REXLOG_INFO("cod3: native C runtime math so far, calls run natively: {}; by the game's code "
              "(arguments the native code leaves to it): {}; compared calls: {}",
              line, left.empty() ? "none" : left,
              differed.empty() ? "all the same" : "differed in " + differed + " (off since)");
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
// The check: arguments of every kind, both ways
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
  double Unit() { return double(Next() >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)
  double Range(double lo, double hi) { return lo + (hi - lo) * Unit(); }
};

struct Arg {
  uint64_t a = 0, b = 0;  // f1 and f2, or r3
};

// Doubles of every size: a random sign, exponent and mantissa (no NaNs unless
// allowed; infinities sometimes).
uint64_t AnyDouble(Rng& rng, bool nans) {
  for (;;) {
    const uint64_t v = rng.Next();
    if (nans || !IsNanBits(v)) return v;
  }
}

// A few steps (ulps) from v either way.
uint64_t Near(Rng& rng, double v) {
  const int64_t step = int64_t(rng.Below(9)) - 4;
  return uint64_t(int64_t(Bits(v)) + step);
}

void TrigArgs(Rng& rng, uint8_t* base, int count, std::vector<Arg>* out) {
  const uint8_t* t = Host(base, kTrigTable);
  const double half_pi = LdDv(t + 0), limit = LdDv(t + 16);
  const double fixed[] = {0.0, 1.0, 0.5, 2.0, 3.0, 1e-300, 1e-10, 1e-5, 0.1, 10.0, 100.0, 1000.0,
                          12345.678, 1e6, 1e9, 1e12, 1e15, 4503599627370496.0, 9007199254740992.0,
                          4.611686018427388e18, 9.223372036854776e18, 1.8446744073709552e19, 1e20,
                          1e100, 1e300, 1.7976931348623157e308, 4.9406564584124654e-324,
                          2.2250738585072009e-308, 2.2250738585072014e-308, INFINITY, half_pi,
                          2 * half_pi, 3 * half_pi, 4 * half_pi, limit};
  for (double v : fixed) {
    out->push_back({Bits(v)});
    out->push_back({Bits(-v)});
    out->push_back({Bits(v) + 1});
    if (Bits(v) > 0) out->push_back({Bits(v) - 1});
  }
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(6);
    double v;
    if (kind == 0) {
      out->push_back({AnyDouble(rng, false)});
      continue;
    } else if (kind == 1) {
      v = double(float(rng.Range(-7.0, 7.0)));  // a float angle
    } else if (kind == 2) {
      v = double(float(rng.Range(-4000.0, 4000.0)));
    } else if (kind == 3) {
      v = rng.Range(-1e6, 1e6);
    } else if (kind == 4) {  // near a multiple of pi/2
      const double k = rng.Below(4) ? double(rng.Below(200)) : double(rng.Below(1u << 30));
      out->push_back({Near(rng, (rng.Below(2) ? -k : k) * half_pi)});
      continue;
    } else {  // near the limit
      out->push_back({Near(rng, rng.Below(2) ? -limit : limit)});
      continue;
    }
    out->push_back({Bits(v)});
  }
}

void FmodArgs(Rng& rng, int count, std::vector<Arg>* out) {
  const uint64_t special[] = {0x0000000000000000ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull,
                              0x0010000000000000ull, 0x3FF0000000000000ull, 0x3FF8000000000000ull,
                              0x4008000000000000ull, 0x3FB999999999999Aull, 0x3F50000000000000ull,
                              0x7E70000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull,
                              0x7FF8000000000000ull, 0x7FF0000000000001ull, 0x4059000000000000ull,
                              0x0008000000000000ull};
  for (uint64_t x : special) {
    for (uint64_t y : special) {
      for (int s = 0; s < 4; ++s) {
        out->push_back({x | ((s & 1) ? kSign : 0), y | ((s & 2) ? kSign : 0)});
      }
    }
  }
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(6);
    if (kind == 0) {  // any bits at all
      out->push_back({rng.Next(), rng.Next()});
    } else if (kind == 1) {  // an exact multiple, exponents far apart
      const uint64_t mant = rng.Next() & 0x000FFFFFFFFFFFFFull;
      const uint64_t ey = rng.Below(2047);
      const uint64_t ex = std::min<uint64_t>(2046, ey + 1 + rng.Below(2046));
      out->push_back({(ex << 52) | mant | (rng.Below(2) ? kSign : 0),
                      (ey << 52) | mant | (rng.Below(2) ? kSign : 0)});
    } else if (kind == 2) {  // everyday values
      out->push_back({Bits(rng.Range(-1e6, 1e6)), Bits(rng.Range(-400.0, 400.0))});
    } else if (kind == 3) {  // angles: fmod(a, 2 pi), fmod(a, 360)
      out->push_back({Bits(rng.Range(-1e5, 1e5)),
                      Bits(rng.Below(2) ? 6.283185307179586 : 360.0)});
    } else if (kind == 4) {  // x a few ulps from a multiple of y
      const double y = rng.Range(0.001, 100.0);
      out->push_back({Near(rng, y * double(1 + rng.Below(100000))), Bits(y)});
    } else {  // finite values of any size
      out->push_back({AnyDouble(rng, false), AnyDouble(rng, false)});
    }
  }
}

void CharArgs(Rng& rng, int lo, int hi, int count, std::vector<Arg>* out) {
  for (int c = lo; c <= hi; ++c) {
    out->push_back({uint64_t(int64_t(c))});
    out->push_back({(rng.Next() & 0xFFFFFFFF00000000ull) | uint32_t(c)});  // junk in the high half
  }
  while (int(out->size()) < count) {
    const int c = lo + int(rng.Below(uint32_t(hi - lo + 1)));
    out->push_back({(rng.Next() & 0xFFFFFFFF00000000ull) | uint32_t(c)});
  }
}

// exp: the limits and the values a few steps from them, multiples of ln 2
// (where n changes), small and huge values, the infinities and NaNs (left to
// the game's code), then random ones across the range and beyond.
void ExpArgs(Rng& rng, uint8_t* base, int count, std::vector<Arg>* out) {
  const uint8_t* t = Host(base, kExpTable);
  const double over = LdDv(t + 0), under = LdDv(t + 8), ln2 = LdDv(t + 96);
  const double fixed[] = {0.0, 1e-300, 4.9406564584124654e-324, 1e-10, 0.5, 1.0, 2.0, 10.0, 100.0,
                          700.0, 708.0, 709.0, 710.0, 745.0, 746.0, 1000.0, 1e10, 1e300,
                          1.7976931348623157e308, INFINITY, over, under, ln2, ln2 / 2};
  for (double v : fixed) {
    for (uint64_t b : {Bits(v), Bits(v) + 1, Bits(v) - 1}) {
      if (v == 0.0 && b == Bits(v) - 1) continue;
      out->push_back({b});
      out->push_back({b ^ kSign});
    }
  }
  for (int k = -1100; k <= 1100; k += 7) {
    out->push_back({Near(rng, k * ln2)});
    out->push_back({Near(rng, (k + 0.5) * ln2)});
  }
  out->push_back({0x7FF8000000000000ull});
  out->push_back({0xFFF8000000000000ull});
  out->push_back({0x7FF0000000000001ull});
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(5);
    if (kind == 0) out->push_back({Bits(rng.Range(-750.0, 750.0))});
    else if (kind == 1) out->push_back({Bits(rng.Range(-2.0, 2.0))});
    else if (kind == 2) out->push_back({Near(rng, rng.Below(2) ? over : under)});
    else if (kind == 3) out->push_back({Bits(double(float(rng.Range(-100.0, 100.0))))});
    else out->push_back({AnyDouble(rng, true)});
  }
}

// log: 1.0 and its neighbors, the sqrt(0.5) split of the mantissa at many
// exponents, zeros, denormals and the smallest normal, the largest finite,
// the infinities, NaNs and negative numbers, then random positive ones of
// every exponent and random bits of every kind.
void LogArgs(Rng& rng, uint8_t* base, int count, std::vector<Arg>* out) {
  const uint8_t* t = Host(base, kLogTable);
  const double split = LdDv(t + 0), tiny = LdDv(t + 0x80), big = LdDv(t + 0x88);
  const double fixed[] = {1.0, 0.0, 0.5, 2.0, 10.0, 1e-300, 4.9406564584124654e-324,
                          2.2250738585072009e-308, tiny, big, INFINITY, 1e300, 3.0, 0.1, 1e10};
  for (double v : fixed) {
    for (uint64_t b : {Bits(v), Bits(v) + 1, Bits(v) - 1}) {
      if (v == 0.0 && b == Bits(v) - 1) continue;
      out->push_back({b});
      out->push_back({b ^ kSign});
    }
  }
  for (int e = -1070; e <= 1023; e += 13) {
    const double s = std::ldexp(split, e);
    out->push_back({Near(rng, s)});
    out->push_back({Near(rng, std::ldexp(1.0, e))});
  }
  out->push_back({0x7FF8000000000000ull});
  out->push_back({0xFFF8000000000000ull});
  out->push_back({0x7FF0000000000001ull});
  out->push_back({0x0000000000000001ull});
  out->push_back({0x000FFFFFFFFFFFFFull});
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(5);
    if (kind == 0) out->push_back({rng.Next() & ~kSign});        // any positive (incl. inf, NaN)
    else if (kind == 1) out->push_back({Bits(rng.Range(0.25, 4.0))});
    else if (kind == 2) out->push_back({Near(rng, 1.0)});
    else if (kind == 3) out->push_back({rng.Next() & 0x000FFFFFFFFFFFFFull});  // denormals
    else out->push_back({AnyDouble(rng, true)});
  }
}

// pow: every pair of a list of edges (zeros, denormals, 1 and its neighbors,
// small integers, the 1e20 and 128 limits, 2^63, the infinities) with both
// signs; then random pairs of every kind: everyday values, x near 1 (where
// pow takes log's way), integer x to integer y up to 140 (the squaring way,
// odd and even, negative x), negative x to fractions (the NaN), results near
// the overflow and underflow limits, floats (powf calls pow), finite values
// of any size, and a few NaNs (left to the game's code).
void PowArgs(Rng& rng, uint8_t* base, int count, std::vector<Arg>* out) {
  (void)base;
  const double xs[] = {0.0, 4.9406564584124654e-324, 1.1125369292536007e-308, 2.2250738585072009e-308,
                       2.2250738585072014e-308, 1e-300, 1e-10, 0.1, 0.5, 0.7071067811865476, 0.9,
                       1.0, 1.1, 1.4142135623730951, 2.0, 2.718281828459045, 3.0, 10.0, 1e10,
                       1e300, 1.7976931348623157e308, INFINITY};
  const double ys[] = {0.0, 4.9406564584124654e-324, 1e-10, 0.25, 0.3333333333333333, 0.5, 1.0, 1.5,
                       2.0, 3.0, 4.0, 7.0, 127.0, 128.0, 129.0, 1021.0, 1022.0, 1024.0, 1025.0, 1074.0,
                       2560.0, 1e20, 1.0000000000000002e20, 1e21, 9.223372036854775807e18,
                       1.8446744073709552e19, 1e300, INFINITY};
  for (double x : xs) {
    for (double y : ys) {
      for (int s = 0; s < 4; ++s) {
        const uint64_t xb = Bits(x) ^ ((s & 1) ? kSign : 0), yb = Bits(y) ^ ((s & 2) ? kSign : 0);
        out->push_back({xb, yb});
      }
    }
  }
  for (double x : xs) {  // one step either side of each edge
    for (double y : {0.5, 2.0, 3.0, 128.0, 1e20}) {
      if (x != 0.0 && x != INFINITY) out->push_back({Bits(x) - 1, Bits(y) + 1});
      if (x != INFINITY) out->push_back({Bits(x) + 1, Bits(y) - 1});
    }
  }
  out->push_back({0x7FF8000000000000ull, Bits(2.0)});
  out->push_back({Bits(2.0), 0xFFF8000000000000ull});
  out->push_back({0x7FF0000000000001ull, 0x0000000000000000ull});
  const size_t target = out->size() + size_t(count);  // count random pairs beyond the edges
  while (out->size() < target) {
    const uint32_t kind = rng.Below(9);
    uint64_t xb, yb;
    if (kind == 0) {  // everyday
      xb = Bits(rng.Range(0.0, 4.0));
      yb = Bits(rng.Range(-50.0, 50.0));
    } else if (kind == 1) {  // x near 1: steps of every size
      const double d = std::ldexp(rng.Unit(), -int(rng.Below(53)));
      xb = Bits(rng.Below(2) ? 1.0 + d : 1.0 - d / 2);
      yb = Bits(rng.Range(-3000.0, 3000.0));
    } else if (kind == 2) {  // integer x to integer y: the squaring way
      const double x = double(int32_t(rng.Below(41)) - 20);
      xb = Bits(rng.Below(8) ? x : std::ldexp(x, int(rng.Below(60))));
      yb = Bits(double(1 + rng.Below(140)));
    } else if (kind == 3) {  // negative x: integer y (either sign) or a fraction
      xb = Bits(-rng.Range(0.0, 100.0));
      const double y = rng.Below(2) ? double(int32_t(rng.Below(400)) - 200) : rng.Range(-10.0, 10.0);
      yb = Bits(y);
    } else if (kind == 4) {  // near the limits: y log2(x) near +-1024, -1021, +-2560
      const double x = rng.Below(2) ? 2.0 : rng.Range(1.5, 1000.0);
      const double lim[] = {1024.0, -1021.0, -1022.0, -1074.0, 2560.0, -2557.0};
      const double k = lim[rng.Below(6)];
      xb = Bits(rng.Below(3) ? x : 1.0 / x);
      yb = Near(rng, k / std::log2(Dbl(xb)));
    } else if (kind == 5) {  // floats (powf)
      xb = Bits(double(float(rng.Range(0.0, 100.0))));
      yb = Bits(double(float(rng.Range(-8.0, 8.0))));
    } else if (kind == 6) {  // positive x of any size, moderate y
      xb = AnyDouble(rng, false) & ~kSign;
      yb = Bits(rng.Range(-2.0, 2.0));
    } else if (kind == 7) {  // denormal x
      xb = (rng.Next() & 0x000FFFFFFFFFFFFFull) | (rng.Below(2) ? kSign : 0);
      yb = Bits(rng.Below(2) ? double(rng.Below(5)) : rng.Range(-1.0, 1.0));
    } else {  // any bits (NaNs now and then)
      xb = AnyDouble(rng, rng.Below(64) == 0);
      yb = AnyDouble(rng, rng.Below(64) == 0);
    }
    out->push_back({xb, yb});
  }
}

// tan: zeros, denormals and the smallest normal (a denormal remainder is left
// to the game's code), the small-remainder bound and the limit with their
// neighbors, the multiples of pi/4 (where n changes) and of pi/2 (the poles,
// where the remainder is tiny and n odd) a few steps either way, the
// infinities and NaNs (left to the game's code); then random everyday angles
// (doubles and floats: tanf callers widen theirs), values up to past the
// limit, values of every size and any double.
void TanArgs(Rng& rng, uint8_t* base, int count, std::vector<Arg>* out) {
  const uint8_t* t = Host(base, kTanTable);
  const double small = LdDv(t + 8), limit = LdDv(t + 16);
  const double half_pi = LdDv(Host(base, kTanHalfPiAddr));
  const double fixed[] = {0.0, 4.9406564584124654e-324, 1e-310, 2.2250738585072009e-308,
                          2.2250738585072014e-308, 1e-300, 1e-20, 1e-10, 1e-5, 0.1, 0.5, 1.0, 2.0,
                          3.0, 10.0, 100.0, 1e4, 1e6, 1e9, 1e12, 1e15, 4503599627370496.0, 1e20,
                          1e300, 1.7976931348623157e308, INFINITY, small, limit, half_pi,
                          0.5 * half_pi};
  for (double v : fixed) {
    for (uint64_t b : {Bits(v), Bits(v) + 1, Bits(v) - 1}) {
      if (v == 0.0 && b == Bits(v) - 1) continue;
      out->push_back({b});
      out->push_back({b ^ kSign});
    }
  }
  for (int k = -200; k <= 200; ++k) {
    const double v = 0.5 * half_pi * k;
    out->push_back({Bits(v)});
    out->push_back({Near(rng, v)});
    out->push_back({Near(rng, v)});
  }
  out->push_back({0x7FF8000000000000ull});
  out->push_back({0xFFF8000000000000ull});
  out->push_back({0x7FF0000000000001ull});
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(7);
    if (kind == 0) {
      out->push_back({Bits(rng.Range(-4.0, 4.0))});
    } else if (kind == 1) {
      out->push_back({Bits(double(float(rng.Range(-4.0, 4.0))))});
    } else if (kind == 2) {
      out->push_back({Bits(rng.Range(-1.01, 1.01) * limit)});
    } else if (kind == 3) {  // near a multiple of pi/2 or pi/4
      const double k = rng.Below(4) ? double(rng.Below(2000)) : double(rng.Below(1u << 30));
      const double v = (rng.Below(2) ? 0.5 : 1.0) * half_pi * (rng.Below(2) ? -k : k);
      out->push_back({Near(rng, v)});
    } else if (kind == 4) {  // a random exponent from 2^-80 up, a random mantissa and sign
      const uint64_t e = uint64_t(1023 - 80 + rng.Below(80 + 64));
      out->push_back({(rng.Next() & (kSign | 0x000FFFFFFFFFFFFFull)) | (e << 52)});
    } else if (kind == 5) {
      out->push_back({Near(rng, rng.Below(2) ? small : limit) ^ (rng.Below(2) ? kSign : 0)});
    } else {
      out->push_back({AnyDouble(rng, true)});
    }
  }
}

// Every kind of double, NaNs and infinities included (the class functions).
void ClassArgs(Rng& rng, int count, std::vector<Arg>* out) {
  const uint64_t edge[] = {0x0000000000000000ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull,
                           0x0008000000000000ull, 0x0010000000000000ull, 0x3FF0000000000000ull,
                           0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0x7FF0000000000001ull,
                           0x7FF7FFFFFFFFFFFFull, 0x7FF8000000000000ull, 0x7FFFFFFFFFFFFFFFull,
                           0x7FF0000100000000ull, 0x7FF0000000080000ull, 0x7FF8000000000001ull};
  for (uint64_t v : edge) {
    out->push_back({v});
    out->push_back({v | kSign});
  }
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(4);
    uint64_t v = rng.Next();
    if (kind == 0) v &= 0x800FFFFFFFFFFFFFull;          // zero exponent: denormals
    else if (kind == 1) v |= 0x7FF0000000000000ull;     // all-ones exponent: NaNs
    out->push_back({v});
  }
}

// floor and ceil: integers and the values around them, the limit, any double.
void RoundArgs(Rng& rng, uint8_t* base, int count, std::vector<Arg>* out) {
  const double limit = LdDv(Host(base, kRoundLimitAddr));
  const double fixed[] = {0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 1e-300, 4.9406564584124654e-324, 1e15,
                          4503599627370496.0, 9007199254740992.0, limit, 9.223372036854776e18,
                          1e19, 1e300, INFINITY};
  for (double v : fixed) {
    for (uint64_t b : {Bits(v), Bits(v) + 1, Bits(v) - 1}) {
      out->push_back({b});
      out->push_back({b ^ kSign});
    }
  }
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(5);
    if (kind == 0) {
      out->push_back({rng.Next()});  // any bits, NaNs too
    } else if (kind == 1) {
      const double k = double(int64_t(rng.Below(2000001)) - 1000000);
      out->push_back({Near(rng, k)});
    } else if (kind == 2) {
      out->push_back({Bits(double(float(rng.Range(-5000.0, 5000.0))))});
    } else if (kind == 3) {
      out->push_back({Near(rng, rng.Below(2) ? -limit : limit)});
    } else {
      out->push_back({Bits(rng.Range(-1e6, 1e6))});
    }
  }
}

// atan and atan2: every size, around 1, 2-sqrt(3) and the limit; atan2's pairs
// with zeros of both signs and infinities.
void AtanArgs(Rng& rng, uint8_t* base, bool two, int count, std::vector<Arg>* out) {
  const uint8_t* t = Host(base, kAtanTable);
  const double spots[] = {0.0, 1.0, LdDv(t + 24), LdDv(t + 48), 1e-300, 4.9406564584124654e-324,
                          0.5, 3.0, 1e10, 1e300, INFINITY};
  if (two) {
    for (double y : spots) {
      for (double x : spots) {
        for (int sgn = 0; sgn < 4; ++sgn) {
          out->push_back({Bits(y) ^ ((sgn & 1) ? kSign : 0), Bits(x) ^ ((sgn & 2) ? kSign : 0)});
        }
      }
    }
  } else {
    for (double v : spots) {
      out->push_back({Bits(v)});
      out->push_back({Bits(v) ^ kSign});
      out->push_back({Near(rng, v)});
    }
  }
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(4);
    auto one = [&]() -> uint64_t {
      if (kind == 0) return AnyDouble(rng, false);
      if (kind == 1) return Bits(double(float(rng.Range(-4.0, 4.0))));
      if (kind == 2) return Near(rng, (rng.Below(2) ? -1.0 : 1.0) * spots[rng.Below(4)]);
      return Bits(rng.Range(-1e4, 1e4));
    };
    const uint64_t a = one();
    out->push_back({a, two ? one() : 0});
  }
}

// asin and _asincos: [-1, 1] (and a little past it), 0.5, the ends; r4 0 or 1
// only: the game's code indexes its table with r4, so another value would make
// it read wherever that points (the game only ever passes 1, from acos).
void AsinArgs(Rng& rng, bool flagged, int count, std::vector<Arg>* out) {
  const double fixed[] = {0.0, 0.5, 1.0, 1e-300, 4.9406564584124654e-324, 0.25, 0.75, 1.5,
                          INFINITY};
  for (double v : fixed) {
    for (uint64_t b : {Bits(v), Bits(v) + 1, Bits(v) - 1}) {
      for (uint64_t f : {0, 1}) {
        out->push_back({b, flagged ? f : 0});
        out->push_back({b ^ kSign, flagged ? f : 0});
      }
    }
  }
  while (int(out->size()) < count) {
    const uint32_t kind = rng.Below(4);
    uint64_t v;
    if (kind == 0) v = Bits(rng.Range(-1.0, 1.0));
    else if (kind == 1) v = Bits(double(float(rng.Range(-1.0, 1.0))));
    else if (kind == 2) v = Near(rng, rng.Below(2) ? 0.5 : 1.0) ^ (rng.Below(2) ? kSign : 0);
    else v = AnyDouble(rng, true);
    out->push_back({v, flagged ? uint64_t(rng.Below(2)) : 0});
  }
}

// Runs the game's code for f on arg in a frame below the caller's (as
// crt_native.cpp does); returns the result register's bits.
uint64_t RunGuest(Fn f, PPCContext& c, uint8_t* base, const Arg& arg) {
  switch (f) {
    case kSin:
      c.f1.u64 = arg.a;
      __imp__sin_guest(c, base);
      return c.f1.u64;
    case kCos:
      c.f1.u64 = arg.a;
      __imp__cos_guest(c, base);
      return c.f1.u64;
    case kFmod:
      c.f1.u64 = arg.a;
      c.f2.u64 = arg.b;
      __imp__fmod_guest(c, base);
      return c.f1.u64;
    case kTolower:
      c.r3.u64 = arg.a;
      __imp__tolower_guest(c, base);
      return c.r3.u64;
    case kToupper:
      c.r3.u64 = arg.a;
      __imp__toupper_guest(c, base);
      return c.r3.u64;
    case kIsalpha:
      c.r3.u64 = arg.a;
      __imp__isalpha_guest(c, base);
      return c.r3.u64;
    case kFpclass:
      c.f1.u64 = arg.a;
      __imp__sub_8234E468(c, base);
      return c.r3.u64;
    case kSptype:
      c.f1.u64 = arg.a;
      __imp__sub_82355AF8(c, base);
      return c.r3.u64;
    case kFloor:
      c.f1.u64 = arg.a;
      __imp__sub_8234F0E0(c, base);
      return c.f1.u64;
    case kCeil:
      c.f1.u64 = arg.a;
      __imp__sub_8234B740(c, base);
      return c.f1.u64;
    case kAtan:
      c.f1.u64 = arg.a;
      __imp__sub_8234EC40(c, base);
      return c.f1.u64;
    case kAtan2:
      c.f1.u64 = arg.a;
      c.f2.u64 = arg.b;
      __imp__sub_8234ED28(c, base);
      return c.f1.u64;
    case kAsin:
      c.f1.u64 = arg.a;
      __imp__sub_8234D010(c, base);
      return c.f1.u64;
    case kAsincos:
      c.f1.u64 = arg.a;
      c.r4.u64 = arg.b;
      __imp__sub_8234CF18(c, base);
      return c.f1.u64;
    case kCopysign:
      c.f1.u64 = arg.a;
      c.f2.u64 = arg.b;
      __imp__sub_8234E430(c, base);
      return c.f1.u64;
    case kIsdigit:
      c.r3.u64 = arg.a;
      __imp__sub_8234BFF8(c, base);
      return c.r3.u64;
    case kIsspace:
      c.r3.u64 = arg.a;
      __imp__sub_8234C038(c, base);
      return c.r3.u64;
    case kIsalnum:
      c.r3.u64 = arg.a;
      __imp__sub_8234C058(c, base);
      return c.r3.u64;
    case kExp:
      c.f1.u64 = arg.a;
      __imp__sub_8234F180(c, base);
      return c.f1.u64;
    case kLog:
      c.f1.u64 = arg.a;
      __imp__sub_8234E980(c, base);
      return c.f1.u64;
    case kPow:
      c.f1.u64 = arg.a;
      c.f2.u64 = arg.b;
      __imp__sub_8234DD18(c, base);
      return c.f1.u64;
    case kTan:
      c.f1.u64 = arg.a;
      __imp__sub_8234BBD8(c, base);
      return c.f1.u64;
    default:
      return 0;
  }
}

// The native result; false when the native code leaves this argument to the
// game's code (a NaN for sin and cos, a table read outside plain memory).
bool RunNative(Fn f, uint8_t* base, const Arg& arg, uint64_t* out) {
  switch (f) {
    case kSin:
      if (IsNanBits(arg.a)) return false;
      *out = NativeSin(base, arg.a);
      return true;
    case kCos:
      if (IsNanBits(arg.a)) return false;
      *out = NativeCos(base, arg.a);
      return true;
    case kFmod:
      *out = NativeFmod(base, arg.a, arg.b);
      return true;
    case kTolower:
      *out = NativeTolower(arg.a);
      return true;
    case kToupper:
      *out = NativeToupper(arg.a);
      return true;
    case kIsalpha:
      return NativeIsalpha(base, arg.a, out);
    case kFpclass:
      *out = NativeFpclass(base, arg.a);
      return true;
    case kSptype:
      *out = NativeSptype(arg.a);
      return true;
    case kFloor:
      *out = NativeFloor(base, arg.a);
      return true;
    case kCeil:
      *out = NativeCeil(base, arg.a);
      return true;
    case kAtan:
      if (IsNanBits(arg.a)) return false;
      *out = NativeAtan(base, arg.a);
      return true;
    case kAtan2:
      return NativeAtan2(base, arg.a, arg.b, out);
    case kAsin:
      return NativeAsincos(base, arg.a, 0, true, out);
    case kAsincos:
      return NativeAsincos(base, arg.a, arg.b, false, out);
    case kCopysign:
      *out = NativeCopysign(arg.a, arg.b);
      return true;
    case kIsdigit:
      return NativeCtype(base, arg.a, kDigitMask, out);
    case kIsspace:
      return NativeCtype(base, arg.a, kSpaceMask, out);
    case kIsalnum:
      return NativeCtype(base, arg.a, kAlnumMask, out);
    case kExp:
      if (IsNanBits(arg.a)) return false;
      *out = NativeExp(base, arg.a);
      return true;
    case kLog:
      *out = NativeLog(base, arg.a);
      return true;
    case kPow:
      if (IsNanBits(arg.a) || IsNanBits(arg.b)) return false;
      return NativePow(base, arg.a, arg.b, g_fn_ok[kLog].load(std::memory_order_relaxed), out);
    case kTan:
      if (!RoundingNearest()) return false;
      return NativeTan(base, arg.a, out);
    default:
      return false;
  }
}

struct FnResult {
  int args = 0, compared = 0;
  bool same = true;
  Arg first_diff;
  uint64_t game = 0, native = 0;
  double game_ns = 0, native_ns = 0;  // a call, on average
};

// count random arguments (plus the fixed edges) for f, both ways.
FnResult CheckFn(Fn f, int count, PPCContext& parent, uint8_t* base, Rng& rng) {
  std::vector<Arg> args;
  args.reserve(size_t(count) + 2048);
  switch (f) {
    case kSin:
    case kCos:
      TrigArgs(rng, base, count, &args);
      break;
    case kFmod:
      FmodArgs(rng, count, &args);
      break;
    case kTolower:
    case kToupper:
      CharArgs(rng, -300, 300, count, &args);
      break;
    case kIsalpha:
    case kIsdigit:
    case kIsspace:
    case kIsalnum:
      CharArgs(rng, -128, 255, count, &args);
      break;
    case kFpclass:
    case kSptype:
      ClassArgs(rng, count, &args);
      break;
    case kExp:
      ExpArgs(rng, base, count, &args);
      break;
    case kLog:
      LogArgs(rng, base, count, &args);
      break;
    case kPow:
      PowArgs(rng, base, count, &args);
      break;
    case kTan:
      TanArgs(rng, base, count, &args);
      break;
    case kFloor:
    case kCeil:
      RoundArgs(rng, base, count, &args);
      break;
    case kAtan:
      AtanArgs(rng, base, false, count, &args);
      break;
    case kAtan2:
      AtanArgs(rng, base, true, count, &args);
      break;
    case kAsin:
      AsinArgs(rng, false, count, &args);
      break;
    case kAsincos:
      AsinArgs(rng, true, count, &args);
      break;
    case kCopysign: {
      std::vector<Arg> a, b;
      ClassArgs(rng, count, &a);
      ClassArgs(rng, count, &b);
      for (size_t i = 0; i < a.size() && i < b.size(); ++i) args.push_back({a[i].a, b[b.size() - 1 - i].a});
      break;
    }
    default:
      break;
  }
  FnResult res;
  res.args = int(args.size());
  std::vector<uint64_t> game(args.size()), native(args.size());
  std::vector<uint8_t> ran(args.size());
  // tan also changes the floating-point state: every other call here starts
  // with the flush mode on, and the state each call leaves (the rounding mode
  // and the flush mode) must be the one the native hook leaves: the rounding
  // mode it found, the flush mode off.
  constexpr uint32_t kStateMask = rex::ppc::FPSCRRegister::GuestMask;
  constexpr uint32_t kRoundBits = uint32_t(rex::ppc::FPSCRRegister::RoundMaskVal);
  std::vector<uint32_t> state_left, state_wanted;
  if (f == kTan) {
    state_left.resize(args.size());
    state_wanted.resize(args.size());
  }
  {
    rex::CallFrame frame(parent);
    PPCContext& c = frame.ctx;
    c.r1.u32 = (parent.r1.u32 - 0x200) & ~0xFu;
    const int64_t t0 = NowNs();
    for (size_t i = 0; i < args.size(); ++i) {
      if (f == kTan) {
        if (i & 1)
          c.fpscr.enableFlushModeUnconditional();
        else
          c.fpscr.disableFlushModeUnconditional();
        state_wanted[i] = c.fpscr.getcsr() & kRoundBits;
      }
      game[i] = RunGuest(f, c, base, args[i]);
      if (f == kTan) state_left[i] = c.fpscr.getcsr() & kStateMask;
    }
    if (f == kTan) c.fpscr.disableFlushModeUnconditional();
    res.game_ns = double(NowNs() - t0) / double(std::max<size_t>(1, args.size()));
  }
  const int64_t t1 = NowNs();
  for (size_t i = 0; i < args.size(); ++i) ran[i] = RunNative(f, base, args[i], &native[i]);
  res.native_ns = double(NowNs() - t1) / double(std::max<size_t>(1, args.size()));
  for (size_t i = 0; i < args.size(); ++i) {
    if (!ran[i]) continue;
    ++res.compared;
    if (game[i] != native[i] && res.same) {
      res.same = false;
      res.first_diff = args[i];
      res.game = game[i];
      res.native = native[i];
    }
    if (f == kTan && state_left[i] != state_wanted[i] && res.same) {
      res.same = false;  // logged as the two states (the host's control bits)
      res.first_diff = args[i];
      res.game = state_left[i];
      res.native = state_wanted[i];
    }
  }
  return res;
}

// 0: not checked yet, 1: a thread is checking now, 2: checked.
std::atomic<int> g_check_state{0};
std::atomic<int> g_live_checks[kFnCount]{};  // the game's first calls, run both ways
constexpr int kLiveChecks = 64;
constexpr uint32_t kSpotEvery = 4096;  // a power of two
std::atomic<int64_t> g_test_pending{0};  // cod3_native_math_test's count, run on a next call

// Runs the check for every function (count random arguments each) and logs it.
// initial: the start-up check (sets which functions run natively).
void RunCheck(PPCContext& ctx, uint8_t* base, int count, bool initial) {
  // As sin and cos do first: the frames below copy this state and give it back.
  ctx.fpscr.disableFlushMode();
  const int64_t start = NowNs();
  Rng rng{initial ? 0x9E3779B97F4A7C15ull : uint64_t(NowNs()) | 1};
  std::string summary;
  int on = 0;
  for (int f = 0; f < kFnCount; ++f) {
    const FnResult r = CheckFn(Fn(f), count, ctx, base, rng);
    if (!r.same) {
      g_fn_ok[f].store(false, std::memory_order_relaxed);
      g_differed[f].store(true, std::memory_order_relaxed);
      REXLOG_ERROR("cod3: native C runtime math: {} differs from the game's own code (arguments "
                   "{:#018x} {:#018x}: the game's code {:#018x}, native {:#018x}); it stays on the "
                   "game's code",
                   kFnName[f], r.first_diff.a, r.first_diff.b, r.game, r.native);
    } else if (initial) {
      g_fn_ok[f].store(r.compared > 0, std::memory_order_relaxed);
    }
    char part[200];
    std::snprintf(part, sizeof(part), "%s%s %d/%d%s (%.0f/%.0f ns)", f ? ", " : "", kFnName[f],
                  r.compared, r.args, r.same ? "" : " DIFFER", r.game_ns, r.native_ns);
    summary += part;
    on += g_fn_ok[f].load(std::memory_order_relaxed) ? 1 : 0;
  }
  const double ms = double(NowNs() - start) / 1e6;
  if (initial) {
    for (int f = 0; f < kFnCount; ++f) g_live_checks[f].store(kLiveChecks, std::memory_order_relaxed);
    REXLOG_INFO("cod3: native C runtime math self-check ({:.1f} ms; arguments compared/made, the "
                "game's code/native ns a call): {}; {} of {} now run as native code "
                "(cod3_native_math)",
                ms, summary, on, int(kFnCount));
    g_next_report_ns.store(NowNs() + kFirstReportNs, std::memory_order_relaxed);
  } else {
    REXLOG_INFO("cod3: native C runtime math test ({:.1f} ms; arguments compared/made, the game's "
                "code/native ns a call): {}; {} of {} run as native code",
                ms, summary, on, int(kFnCount));
  }
}

void TestCommand(std::string_view args) {
  int64_t n = 1000000;
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
  n = std::clamp<int64_t>(n, 1000, 2000000);
  g_test_pending.store(n, std::memory_order_relaxed);
  REXLOG_INFO("cod3: native C runtime math test: {} random arguments per function on the next call "
              "of one of them (the game pauses while it runs)",
              n);
}

// Called by a hook until the check has run. True once it ran (on this or
// another thread); while another thread runs it, false (that call takes the
// game's code).
bool EnsureChecked(PPCContext& ctx, uint8_t* base) {
  int expected = 0;
  if (!g_check_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    return expected == 2;
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory() || ks->memory()->virtual_membase() != base ||
      !Plain(base, kTrigTable, kTrigTableSize) || !Plain(base, kOneAddr, 8) ||
      !Plain(base, kNanAddr, 8) || !Plain(base, kCtypePointer, 4) ||
      !Plain(base, kAtanTable, kAtanTableSize) || !Plain(base, kAsinTable, kAsinTableSize) ||
      !Plain(base, kRoundLimitAddr, 8) || !Plain(base, kZeroAddr, 8) ||
      !Plain(base, kExpTable, kExpTableSize) || !Plain(base, kLogTable, kLogTableSize) ||
      !Plain(base, kInfAddr, 8) || !Plain(base, kHalfAddr, 8) ||
      !Plain(base, kPowTable, kPowTableSize) || !Plain(base, kMinusOneAddr, 8) ||
      !Plain(base, kPowHugeYAddr, 8) || !Plain(base, kPowIntYAddr, 8) || !Plain(base, kTwoAddr, 8) ||
      !Plain(base, kSixteenAddr, 8) || !Plain(base, kMinusZeroAddr, 8) ||
      !Plain(base, kTanTable, kTanTableSize) || !Plain(base, kTanHalfPiAddr, 16) ||
      !Plain(base, kTanRoundAddr, 16)) {
    REXLOG_WARN("cod3: native C runtime math off: guest memory isn't where the game's code "
                "expects it");
    g_check_state.store(2, std::memory_order_release);  // all stay off
    return true;
  }
  {
    const uint8_t* t = Host(base, kTanTable);
    const uint8_t* h = Host(base, kTanHalfPiAddr);
    const uint8_t* k = Host(base, kTanRoundAddr);
    REXLOG_INFO("cod3: tan's constants (read where its code reads them): scale {:.17g}, small "
                "remainder {:.17g}, limit {:.17g}, pi/2 {:.17g} + {:.17g}, rounding {:.17g} and "
                "{:.17g}",
                LdDv(t + 0), LdDv(t + 8), LdDv(t + 16), LdDv(h + 0), LdDv(h + 8), LdDv(k + 0),
                LdDv(k + 8));
  }
  RunCheck(ctx, base, 3000, true);
  g_check_state.store(2, std::memory_order_release);
  return true;
}

inline bool Use(Fn f, PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cod3_native_math)) return false;
  if (g_check_state.load(std::memory_order_acquire) != 2 && !EnsureChecked(ctx, base))
    return false;
  if (g_test_pending.load(std::memory_order_relaxed) != 0) {
    const int64_t n = g_test_pending.exchange(0, std::memory_order_relaxed);
    if (n > 0) RunCheck(ctx, base, int(n), false);
  }
  return g_fn_ok[f].load(std::memory_order_relaxed);
}

// Whether this call runs both ways: the game's first calls, then one in
// kSpotEvery per thread (every call with cod3_native_math_check_all).
inline bool Checking(Fn f, Counters& c) {
  if (g_live_checks[f].load(std::memory_order_relaxed) > 0 &&
      g_live_checks[f].fetch_sub(1, std::memory_order_relaxed) > 0)
    return true;
  if (((++c.tick[f]) & (kSpotEvery - 1)) == 0) {
    MaybeReport();
    return true;
  }
  return REXCVAR_GET(cod3_native_math_check_all);
}

// After the game's code ran a checked call (its result stays): compares it.
void Compared(Fn f, Counters& c, const Arg& arg, uint64_t native, uint64_t game) {
  Bump(c.checked[f]);
  if (native == game) return;
  g_fn_ok[f].store(false, std::memory_order_relaxed);
  if (!g_differed[f].exchange(true)) {
    REXLOG_ERROR("cod3: native C runtime math: {} differed from the game's own code in a call "
                 "(arguments {:#018x} {:#018x}: the game's code {:#018x}, native {:#018x}); it "
                 "runs the game's code from now on",
                 kFnName[f], arg.a, arg.b, game, native);
  }
}

// The hook body of the functions below: native where Use() and the function's
// own rules allow (RunNative), else the original; a checked call runs the
// original too and keeps its result. *result is the register the function
// returns in (f1 or r3). Their generated code switches the FPU's flush mode
// off first, so this does too.
template <typename Guest>
inline void RunHooked(Fn f, PPCContext& ctx, uint8_t* base, const Arg& arg, uint64_t* result,
                      Guest guest) {
  if (Use(f, ctx, base)) {
    Counters& c = Mine();
    ctx.fpscr.disableFlushMode();
    uint64_t r = 0;
    if (RunNative(f, base, arg, &r)) {
      if (Checking(f, c)) {
        guest(ctx, base);
        Compared(f, c, arg, r, *result);
        return;
      }
      *result = r;
      Bump(c.native[f]);
      return;
    }
    Bump(c.guest[f]);
  }
  guest(ctx, base);
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_math_report, ReportCommand, "cod3/Performance",
                            "cod3_native_math_report: how many calls of sin, cos, fmod, tolower, "
                            "toupper, the ctype functions and the others ran as native code, "
                            "written to the log");
REXCVAR_DEFINE_COMMAND_ARGS(cod3_native_math_test, TestCommand, "cod3/Performance",
                            "cod3_native_math_test N: compares N more random arguments per "
                            "function (1000 to 2000000, default 1000000) with the game's code on "
                            "the next call of one of them, written to the log");

// ---------------------------------------------------------------------------
// The hooks: native where Use() allows, else the original code. A checked call
// runs the original too and keeps its result.
// ---------------------------------------------------------------------------

REX_HOOK_RAW(sin_guest) {
  if (Use(kSin, ctx, base)) {
    Counters& c = Mine();
    const Arg arg{ctx.f1.u64};
    if (!IsNanBits(arg.a)) {
      ctx.fpscr.disableFlushMode();  // as the game's code does first
      const uint64_t r = NativeSin(base, arg.a);
      if (Checking(kSin, c)) {
        __imp__sin_guest(ctx, base);
        Compared(kSin, c, arg, r, ctx.f1.u64);
        return;
      }
      ctx.f1.u64 = r;
      Bump(c.native[kSin]);
      return;
    }
    Bump(c.guest[kSin]);
  }
  __imp__sin_guest(ctx, base);
}

REX_HOOK_RAW(cos_guest) {
  if (Use(kCos, ctx, base)) {
    Counters& c = Mine();
    const Arg arg{ctx.f1.u64};
    if (!IsNanBits(arg.a)) {
      ctx.fpscr.disableFlushMode();  // as the game's code does first
      const uint64_t r = NativeCos(base, arg.a);
      if (Checking(kCos, c)) {
        __imp__cos_guest(ctx, base);
        Compared(kCos, c, arg, r, ctx.f1.u64);
        return;
      }
      ctx.f1.u64 = r;
      Bump(c.native[kCos]);
      return;
    }
    Bump(c.guest[kCos]);
  }
  __imp__cos_guest(ctx, base);
}

REX_HOOK_RAW(fmod_guest) {
  if (Use(kFmod, ctx, base)) {
    Counters& c = Mine();
    const Arg arg{ctx.f1.u64, ctx.f2.u64};
    ctx.fpscr.disableFlushMode();  // as the game's code does first
    const uint64_t r = NativeFmod(base, arg.a, arg.b);
    if (Checking(kFmod, c)) {
      __imp__fmod_guest(ctx, base);
      Compared(kFmod, c, arg, r, ctx.f1.u64);
      return;
    }
    ctx.f1.u64 = r;
    Bump(c.native[kFmod]);
    return;
  }
  __imp__fmod_guest(ctx, base);
}

REX_HOOK_RAW(tolower_guest) {
  if (Use(kTolower, ctx, base)) {
    Counters& c = Mine();
    const Arg arg{ctx.r3.u64};
    const uint64_t r = NativeTolower(arg.a);
    if (Checking(kTolower, c)) {
      __imp__tolower_guest(ctx, base);
      Compared(kTolower, c, arg, r, ctx.r3.u64);
      return;
    }
    ctx.r3.u64 = r;
    Bump(c.native[kTolower]);
    return;
  }
  __imp__tolower_guest(ctx, base);
}

REX_HOOK_RAW(toupper_guest) {
  if (Use(kToupper, ctx, base)) {
    Counters& c = Mine();
    const Arg arg{ctx.r3.u64};
    const uint64_t r = NativeToupper(arg.a);
    if (Checking(kToupper, c)) {
      __imp__toupper_guest(ctx, base);
      Compared(kToupper, c, arg, r, ctx.r3.u64);
      return;
    }
    ctx.r3.u64 = r;
    Bump(c.native[kToupper]);
    return;
  }
  __imp__toupper_guest(ctx, base);
}

REX_HOOK_RAW(isalpha_guest) {
  if (Use(kIsalpha, ctx, base)) {
    Counters& c = Mine();
    const Arg arg{ctx.r3.u64};
    uint64_t r = 0;
    if (NativeIsalpha(base, arg.a, &r)) {
      if (Checking(kIsalpha, c)) {
        __imp__isalpha_guest(ctx, base);
        Compared(kIsalpha, c, arg, r, ctx.r3.u64);
        return;
      }
      ctx.r3.u64 = r;
      ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);  // andi. sets cr0 too
      Bump(c.native[kIsalpha]);
      return;
    }
    Bump(c.guest[kIsalpha]);
  }
  __imp__isalpha_guest(ctx, base);
}

REX_HOOK_RAW(sub_8234E468) {  // _fpclass
  RunHooked(kFpclass, ctx, base, Arg{ctx.f1.u64}, &ctx.r3.u64, __imp__sub_8234E468);
}

REX_HOOK_RAW(sub_82355AF8) {  // _sptype
  RunHooked(kSptype, ctx, base, Arg{ctx.f1.u64}, &ctx.r3.u64, __imp__sub_82355AF8);
}

REX_HOOK_RAW(sub_8234F0E0) {  // floor
  RunHooked(kFloor, ctx, base, Arg{ctx.f1.u64}, &ctx.f1.u64, __imp__sub_8234F0E0);
}

REX_HOOK_RAW(sub_8234B740) {  // ceil
  RunHooked(kCeil, ctx, base, Arg{ctx.f1.u64}, &ctx.f1.u64, __imp__sub_8234B740);
}

REX_HOOK_RAW(sub_8234EC40) {  // atan
  RunHooked(kAtan, ctx, base, Arg{ctx.f1.u64}, &ctx.f1.u64, __imp__sub_8234EC40);
}

REX_HOOK_RAW(sub_8234ED28) {  // atan2 (y, x)
  RunHooked(kAtan2, ctx, base, Arg{ctx.f1.u64, ctx.f2.u64}, &ctx.f1.u64, __imp__sub_8234ED28);
}

REX_HOOK_RAW(sub_8234D010) {  // asin
  RunHooked(kAsin, ctx, base, Arg{ctx.f1.u64}, &ctx.f1.u64, __imp__sub_8234D010);
}

REX_HOOK_RAW(sub_8234CF18) {  // _asincos (x, r4: 1 for acos)
  RunHooked(kAsincos, ctx, base, Arg{ctx.f1.u64, ctx.r4.u64}, &ctx.f1.u64, __imp__sub_8234CF18);
}

REX_HOOK_RAW(sub_8234E430) {  // _copysign (x, y)
  RunHooked(kCopysign, ctx, base, Arg{ctx.f1.u64, ctx.f2.u64}, &ctx.f1.u64, __imp__sub_8234E430);
}

// isdigit, isspace and isalnum (setup tool 2.12.7): as isalpha_guest above;
// only isalnum's andi. sets cr0.
namespace {
template <typename Guest>
inline void CtypeHook(Fn f, uint32_t mask, bool sets_cr0, PPCContext& ctx, uint8_t* base,
                      Guest guest) {
  if (Use(f, ctx, base)) {
    Counters& c = Mine();
    const Arg arg{ctx.r3.u64};
    uint64_t r = 0;
    if (NativeCtype(base, arg.a, mask, &r)) {
      if (Checking(f, c)) {
        guest(ctx, base);
        Compared(f, c, arg, r, ctx.r3.u64);
        return;
      }
      ctx.r3.u64 = r;
      if (sets_cr0) ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
      Bump(c.native[f]);
      return;
    }
    Bump(c.guest[f]);
  }
  guest(ctx, base);
}
}  // namespace

REX_HOOK_RAW(sub_8234BFF8) {  // isdigit
  CtypeHook(kIsdigit, kDigitMask, false, ctx, base, __imp__sub_8234BFF8);
}

REX_HOOK_RAW(sub_8234C038) {  // isspace
  CtypeHook(kIsspace, kSpaceMask, false, ctx, base, __imp__sub_8234C038);
}

REX_HOOK_RAW(sub_8234C058) {  // isalnum
  CtypeHook(kIsalnum, kAlnumMask, true, ctx, base, __imp__sub_8234C058);
}

// exp and log (setup tool 2.12.8).
REX_HOOK_RAW(sub_8234F180) {  // exp
  RunHooked(kExp, ctx, base, Arg{ctx.f1.u64}, &ctx.f1.u64, __imp__sub_8234F180);
}

REX_HOOK_RAW(sub_8234E980) {  // log
  RunHooked(kLog, ctx, base, Arg{ctx.f1.u64}, &ctx.f1.u64, __imp__sub_8234E980);
}

// pow (setup tool 2.12.11).
REX_HOOK_RAW(sub_8234DD18) {  // pow (x, y)
  RunHooked(kPow, ctx, base, Arg{ctx.f1.u64, ctx.f2.u64}, &ctx.f1.u64, __imp__sub_8234DD18);
}

// tan (setup tool 2.12.16). Its code switches the flush mode off first (the
// stfd of f30), then reads the control word (mffs, in _ctrlfp) and sets
// round-to-nearest for its arithmetic; at the end it puts the old mode back
// (mffs and mtfsf again). So the native version runs only when the mode is
// already round-to-nearest, and leaves the state as the game's code does: the
// flush mode off, the mode as it was, the cached word read from the host.
REX_HOOK_RAW(sub_8234BBD8) {  // tan
  if (Use(kTan, ctx, base)) {
    Counters& c = Mine();
    const Arg arg{ctx.f1.u64};
    ctx.fpscr.disableFlushMode();                                         // stfd f30,-40(r1)
    const bool nearest = ctx.fpscr.loadFromHost() == rex::ppc::kRoundNearest;  // mffs f0
    uint64_t r = 0;
    if (nearest && NativeTan(base, arg.a, &r)) {
      if (Checking(kTan, c)) {
        __imp__sub_8234BBD8(ctx, base);
        Compared(kTan, c, arg, r, ctx.f1.u64);
        return;
      }
      ctx.f1.u64 = r;
      ctx.fpscr.loadFromHost();  // the restore's mffs: the cached word is the host's again
      Bump(c.native[kTan]);
      return;
    }
    Bump(c.guest[kTan]);
  }
  __imp__sub_8234BBD8(ctx, base);
}
