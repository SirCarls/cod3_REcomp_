// PowerPC arithmetic the game relies on, where C++'s own operators differ.
// Twin of engine/crates/cod3_game/src/ppc.rs. [acct2-decomp]
//
// Plain `float` arithmetic (+ - * /, std::sqrt) already rounds like the
// PowerPC's single-precision instructions (fadds, fmuls, fdivs, fsqrts) on
// any build whose floats are evaluated in their own precision (checked below:
// no x87 extended precision). What needs care:
//
// * float to integer conversions: fctiwz truncates and saturates, and turns
//   NaN into INT32_MIN, where a C++ cast of an out-of-range value is undefined;
// * fused multiply-add (fmadds, fmsubs, fnmsubs): one rounding, never
//   a * b + c. fmadds here is exact on every platform with double arithmetic
//   only, so it needs no C library and gives the same bits everywhere;
// * a NaN made by the arithmetic itself (infinity - infinity ...): the
//   PowerPC's is 0x7FC00000, x86's 0xFFC00000 (fadds below, where it matters).
#pragma once

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>

static_assert(FLT_EVAL_METHOD == 0,
              "the decomp needs floats computed in their own precision (a 64-bit or SSE2 build, not x87)");
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559,
              "the decomp needs IEEE 754 floats");

namespace cod3::ppc {

// fctiwz: double to a 32-bit integer, rounding toward zero. Out-of-range values
// saturate to INT32_MIN / INT32_MAX; NaN becomes INT32_MIN (0x80000000), as the
// PowerPC Book I specifies.
inline std::int32_t fctiwz(double value) {
    if (!(value > -2147483649.0)) {  // below the range, or NaN
        return std::numeric_limits<std::int32_t>::min();
    }
    if (!(value < 2147483648.0)) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(value);  // truncates toward zero
}

// fctidz: double to a 64-bit integer, rounding toward zero (NaN -> INT64_MIN).
inline std::int64_t fctidz(double value) {
    if (std::isnan(value) || value < -9223372036854775808.0) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (value >= 9223372036854775808.0) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

// The nearest whole number, ties to even, whatever the program's rounding mode is.
inline double round_ties_even(double value) {
    double whole = std::floor(value);
    const double fraction = value - whole;  // exact; NaN for infinities, which stay as they are
    if (fraction > 0.5 || (fraction == 0.5 && std::fmod(whole, 2.0) != 0.0)) {
        whole += 1.0;
    }
    return whole;
}

// fctid in the default rounding mode: double to a 64-bit integer, rounding to
// nearest (ties to even), saturating, NaN -> INT64_MIN.
inline std::int64_t fctid(double value) {
    if (std::isnan(value)) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return fctidz(round_ties_even(value));
}

// fsel: a >= 0.0 ? if_true : otherwise. -0.0 counts as >= 0; NaN picks otherwise.
inline double fsel(double a, double if_true, double otherwise) {
    return a >= 0.0 ? if_true : otherwise;
}

// fmadds: a * b + c rounded once to float, with double arithmetic only.
//
// The product of two floats is exact in a double (24 + 24 bits fit in 53). The
// sum can't be exact in general, and rounding it to double and then to float
// could round twice the wrong way. So the sum is rounded "to odd" instead:
// when it isn't exact, the double whose last bit is 1 is taken (of the two
// around the exact sum). A double rounded that way still rounds to the right
// float, since it carries 29 more bits than a float needs (Boldo and
// Melquiond, "When double rounding is odd", 2005). The rounding error of the
// double sum is found exactly with Knuth's two-sum. (This needs the compiler
// not to fuse anything here: -ffp-contract=off, CMakeLists.txt.)
inline float fmadds(float a, float b, float c) {
    const double product = static_cast<double>(a) * static_cast<double>(b);
    const double addend = static_cast<double>(c);
    const double sum = product + addend;
    if (!std::isfinite(sum)) {
        return static_cast<float>(sum);  // infinities and NaN pass through
    }
    // Two-sum: error is exactly (product + addend) - sum.
    const double addend_part = sum - product;
    const double error = (product - (sum - addend_part)) + (addend - addend_part);
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(sum);
    if (error == 0.0 || (bits & 1u) == 1u) {
        return static_cast<float>(sum);  // exact, or already odd
    }
    // Inexact and even: step one unit toward the exact sum, which is odd.
    const bool away_from_zero = (error > 0.0) == (sum > 0.0);
    const std::uint64_t odd = away_from_zero ? bits + 1u : bits - 1u;
    return static_cast<float>(std::bit_cast<double>(odd));
}

// fmsubs: a * b - c with a single rounding.
inline float fmsubs(float a, float b, float c) { return fmadds(a, b, -c); }

// fnmsubs: -(a * b - c), that is c - a * b, with a single rounding.
inline float fnmsubs(float a, float b, float c) { return -fmsubs(a, b, c); }

// fadds: a + b with the PowerPC's NaNs. A NaN operand comes out quieted (a's
// first, as frA), and infinity + -infinity gives the default NaN 0x7FC00000;
// x86 would give 0xFFC00000 for that, and the compiler may swap a and b.
inline float fadds(float a, float b) {
    constexpr std::uint32_t quiet_bit = 0x00400000u;
    if (std::isnan(a)) {
        return std::bit_cast<float>(std::bit_cast<std::uint32_t>(a) | quiet_bit);
    }
    if (std::isnan(b)) {
        return std::bit_cast<float>(std::bit_cast<std::uint32_t>(b) | quiet_bit);
    }
    const float sum = a + b;
    return std::isnan(sum) ? std::bit_cast<float>(0x7FC00000u) : sum;
}

// A float from its bit pattern, for constants written as the game stores them.
constexpr float f32_bits(std::uint32_t bits) { return std::bit_cast<float>(bits); }
// A double from its bit pattern.
constexpr double f64_bits(std::uint64_t bits) { return std::bit_cast<double>(bits); }

}  // namespace cod3::ppc
