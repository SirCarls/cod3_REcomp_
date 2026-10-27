// [acct2-decomp] The Xbox 360 C runtime's sin and cos (xbox_crt.h), from the
// recompiled C++ (sin_guest, cos_guest) and the checked Rust port
// (engine/crates/cod3_game/src/math/crt.rs); and its _vsnprintf's rules.
#include "cod3/xbox_crt.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cod3/ppc.h"

namespace cod3::xbox_crt {

using ppc::f64_bits;

static constexpr double kHalfPi = f64_bits(0x3FF921FB54442D18u);      // pi / 2
static constexpr double kInvPi = f64_bits(0x3FD45F306DC9C883u);       // 1 / pi
static constexpr double kPiHead = f64_bits(0x400921FB54400000u);      // pi's first 34 bits: n * kPiHead is exact
static constexpr double kPiTail = f64_bits(0x3DE0B4611A600000u);      // pi - kPiHead
static constexpr double kRangeLimit = f64_bits(0x41AA39DE00000000u);  // 2.2e8: beyond it every bit is lost
static constexpr double kOutOfRange = f64_bits(0xFFF8000000000000u);  // the NaN returned beyond it

// The odd polynomial's coefficients for r^3, r^5 ... r^17 (close to +-1/n!).
static constexpr double kC3 = f64_bits(0xBFC5555555555555u);   // -1/6
static constexpr double kC5 = f64_bits(0x3F811111111110B0u);   // 1/120
static constexpr double kC7 = f64_bits(0xBF2A01A01A013E1Au);
static constexpr double kC9 = f64_bits(0x3EC71DE3A524F063u);
static constexpr double kC11 = f64_bits(0xBE5AE6454B5DC0ABu);
static constexpr double kC13 = f64_bits(0x3DE6123C686AD430u);
static constexpr double kC15 = f64_bits(0xBD6AE420DC08499Cu);
static constexpr double kC17 = f64_bits(0x3CE880FF6993DF95u);

// sin(r) for |r| <= pi/2: r * (1 + z*(c3 + z*(c5 + ... + z*c17))), z = r*r, every step fused (fmadd).
static double OddPolynomial(double r) {
    const double z = r * r;
    double p = kC17;
    p = std::fma(p, z, kC15);
    p = std::fma(p, z, kC13);
    p = std::fma(p, z, kC11);
    p = std::fma(p, z, kC9);
    p = std::fma(p, z, kC7);
    p = std::fma(p, z, kC5);
    p = std::fma(p, z, kC3);
    return std::fma(p, z, 1.0) * r;
}

// x - n * pi in two fused steps (fnmsub), n a whole number.
static double Reduce(double x, double n) {
    const double r = std::fma(-kPiHead, n, x);
    return std::fma(-kPiTail, n, r);
}

double sin(double x) {
    const double ax = std::fabs(x);
    const double sign = ppc::fsel(x, 1.0, -1.0);
    const double n = static_cast<double>(ppc::fctid(kInvPi * ax));  // the nearest multiple of pi
    double s = OddPolynomial(Reduce(ax, n));
    if (ppc::fctidz(n) & 1) {
        s = -s;  // an odd multiple of pi turns the sign
    }
    const double result = ppc::fsel(ax - kRangeLimit, kOutOfRange, s * sign);
    if (std::bit_cast<std::uint64_t>(ax) == 0) {
        return x;  // +0 and -0 come back as they went in
    }
    return result;
}

double cos(double x) {
    const double ax = std::fabs(x);
    const double shifted = kHalfPi + ax;
    const double n = static_cast<double>(ppc::fctid(kInvPi * shifted));
    double c = OddPolynomial(Reduce(ax, n - 0.5));
    if (ppc::fctidz(n) & 1) {
        c = -c;
    }
    const double result = ppc::fsel(shifted - kRangeLimit, kOutOfRange, c);
    return ax != 0.0 ? result : 1.0;
}

int _vsnprintf(char* buffer, std::size_t count, const char* format, std::va_list args) {
    // The formatting itself is the C library's (%s and %d give the same text
    // everywhere); only what reaches the buffer follows Microsoft's rules.
    std::va_list measure;
    va_copy(measure, args);
    const int length = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (length < 0) {
        return -1;
    }
    std::vector<char> text(static_cast<std::size_t>(length) + 1);
    std::vsnprintf(text.data(), text.size(), format, args);
    const std::size_t written = std::min(static_cast<std::size_t>(length), count);
    std::memcpy(buffer, text.data(), written);
    if (written < count) {
        buffer[written] = '\0';
    }
    return static_cast<std::size_t>(length) < count ? length : -1;
}

}  // namespace cod3::xbox_crt
