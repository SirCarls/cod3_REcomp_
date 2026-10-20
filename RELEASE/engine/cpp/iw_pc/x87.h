// The x87 FPU at Call of Duty 2's precision: 24-bit significands, the x87's own exponent
// range (see the Rust version, iw_pc::x87). F24 is a register value: a double rounded to 24
// significant bits after every operation; store() is `fstp dword ptr`. NaNs follow the x87's
// rules (the larger significand, then the positive one; invalid operations give the negative
// default NaN).
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace iw_pc::x87 {

namespace detail {
// hi + lo (an exact pair) rounded to 24 significant bits, ties to even, keeping hi's range.
inline double round24(double hi, double lo) {
    if (!std::isfinite(hi) || hi == 0.0) return hi;
    constexpr unsigned kDrop = 52 - 23;
    std::uint64_t bits = std::bit_cast<std::uint64_t>(hi);
    std::uint64_t low = bits & ((std::uint64_t{1} << kDrop) - 1);
    std::uint64_t half = std::uint64_t{1} << (kDrop - 1);
    std::uint64_t kept = bits >> kDrop;
    bool up;
    if (low > half) up = true;
    else if (low < half) up = false;
    else if (lo == 0.0 || std::isnan(lo)) up = (kept & 1) != 0;
    else up = (lo > 0.0) == (hi > 0.0);
    return std::bit_cast<double>((kept + (up ? 1 : 0)) << kDrop);
}
// The x87's default NaN ("real indefinite"): negative, quiet.
inline double indefinite() { return std::bit_cast<double>(std::uint64_t{0xFFF8000000000000}); }

// The NaN an operation on a and b gives when either is one (the x87's rules, see the Rust
// version); false when neither is.
inline bool propagate(double a, double b, double& out) {
    auto quiet = [](double x) { return std::bit_cast<double>(std::bit_cast<std::uint64_t>(x) | 0x0008000000000000); };
    bool na = std::isnan(a), nb = std::isnan(b);
    if (!na && !nb) return false;
    if (na && !nb) { out = quiet(a); return true; }
    if (!na && nb) { out = quiet(b); return true; }
    double qa = quiet(a), qb = quiet(b);
    auto mantissa = [](double x) { return std::bit_cast<std::uint64_t>(x) & 0x000FFFFFFFFFFFFF; };
    if (mantissa(qa) > mantissa(qb)) out = qa;
    else if (mantissa(qa) < mantissa(qb)) out = qb;
    else out = std::signbit(qa) ? qb : qa;
    return true;
}

// An operation's result: the NaN rules, else exact(a, b).
template <class F>
double op(double a, double b, F exact) {
    double nan;
    if (propagate(a, b, nan)) return nan;
    double r = exact(a, b);
    return std::isnan(r) ? indefinite() : r;
}
}  // namespace detail

struct F24 {
    double v = 0.0;

    static F24 load(float x) { return {static_cast<double>(x)}; }       // fld dword ptr
    static F24 integer(std::int32_t x) { return {static_cast<double>(x)}; }  // fild: exact
    static F24 dbl(double x) { return {x}; }                              // fld qword ptr
    float store() const { return static_cast<float>(v); }                 // fstp dword ptr
    F24 sqrt() const { return {detail::op(v, 0.0, [](double a, double) { return detail::round24(std::sqrt(a), 0.0); })}; }
    bool is_nan() const { return std::isnan(v); }
    F24 abs() const { return {std::fabs(v)}; }  // fabs: the sign cleared (a NaN's too)
    /// fistp dword ptr in the default rounding (nearest, ties to even); NaN and out of range: INT32_MIN.
    std::int32_t round_to_int() const {
        double r = std::nearbyint(v);  // (the default rounding mode: to nearest, ties to even)
        if (std::isnan(r) || r < -2147483648.0 || r > 2147483647.0) return INT32_MIN;
        return static_cast<std::int32_t>(r);
    }

    friend F24 operator+(F24 a, F24 b) {
        return {detail::op(a.v, b.v, [](double x, double y) {
            double s = x + y;
            if (!std::isfinite(s)) return s;
            double bb = s - x;
            double err = (x - (s - bb)) + (y - bb);
            return detail::round24(s, err);
        })};
    }
    friend F24 operator-(F24 a) { return {-a.v}; }
    friend F24 operator-(F24 a, F24 b) {  // (not a + -b: a NaN keeps its own sign)
        return {detail::op(a.v, b.v, [](double x, double y) {
            double s = x - y;
            if (!std::isfinite(s)) return s;
            double ny = -y;
            double bb = s - x;
            double err = (x - (s - bb)) + (ny - bb);
            return detail::round24(s, err);
        })};
    }
    friend F24 operator*(F24 a, F24 b) {
        return {detail::op(a.v, b.v, [](double x, double y) {
            double p = x * y;
            if (!std::isfinite(p) || p == 0.0) return p;
            return detail::round24(p, std::fma(x, y, -p));
        })};
    }
    friend F24 operator/(F24 a, F24 b) {
        return {detail::op(a.v, b.v, [](double x, double y) { return detail::round24(x / y, 0.0); })};
    }
    friend bool operator<(F24 a, F24 b) { return a.v < b.v; }
    friend bool operator>(F24 a, F24 b) { return a.v > b.v; }
    friend bool operator<=(F24 a, F24 b) { return a.v <= b.v; }
    friend bool operator>=(F24 a, F24 b) { return a.v >= b.v; }
};

}  // namespace iw_pc::x87
