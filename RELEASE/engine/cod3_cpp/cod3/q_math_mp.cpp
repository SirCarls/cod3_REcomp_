// The multiplayer program's own math (codmp_xenonf.xex): AngleWrap180 and the
// fast yaw. [acct2-decomp]
//
// Written from the recompiled C++ (recomp/mp/generated; names in q_shared.h)
// and the checked Rust ports (engine/crates/cod3_game/src/math/{angles,yaw}.rs).
//
// The campaign's vectoyaw is Quake III's (atan2). The multiplayer program's
// works it out in single precision without the C runtime: it scales (x, y) to
// length 1, takes the arcsine of the smaller of |x| and |y| from a short
// series and puts the angle in the right quadrant. Its result goes into
// position messages (8 bits of movement direction), so every program in a
// match must compute the same bits; the fused steps are written out
// (ppc::fmadds ...) with the source expression next to them.
#include <cmath>

#include "cod3/q_shared.h"

namespace cod3 {

using ppc::f32_bits;
using ppc::fmadds;
using ppc::fmsubs;
using ppc::fnmsubs;

void AngleWrap180(float* out, const float* angle) {
    const float a = *angle;
    // Two comparisons, not a range test: a NaN stays out of the fmod, as on the console.
    if (a < -180.0f || a > 180.0f) {
        *out = static_cast<float>(std::fmod(static_cast<double>(a + 180.0f), 360.0)) - 180.0f;
    } else {
        *out = a;
    }
}

// The constants as the multiplayer program stores them.
static constexpr float kHalfPi = f32_bits(0x3FC90FDBu);           // pi / 2
static constexpr float kHalfPiLow = f32_bits(0x3FC90FD8u);        // pi / 2, three steps low: the original's
static constexpr float kPi = f32_bits(0x40490FDBu);               // pi
static constexpr float kRadiansToDegrees = f32_bits(0x42652EE0u); // 180 / pi

// asin(s) for 0 <= s <= 1.
//   Below 0.5 the Taylor series: s + s^3/6 + 0.075 s^5 + 0.054 s^7.
//   Above it asin(s) = pi/2 - 2 asin(r), r = sqrt((1 - s) / 2), with the same
//   series in r: pi/2 - 2r - r^3/3 - 0.15 r^5 - 0.108 r^7.
static float ArcSine(float s) {
    if (s < 0.5f) {
        const float s2 = s * s;
        const float s3 = s2 * s;
        const float s5 = s3 * s2;
        const float s7 = s5 * s2;
        // s3 * 0.1666667 + s7 * 0.05398124 + s5 * 0.075, summed from the right, fused; then + s.
        return fmadds(s3, f32_bits(0x3E2AAAADu), fmadds(s7, f32_bits(0x3D5D1B6Fu), s5 * f32_bits(0x3D99999Au))) + s;
    }
    const float r = std::sqrt(std::fabs((1.0f - s) * 0.5f));
    const float r2 = r * r;
    const float r3 = r2 * r;
    const float r5 = r3 * r2;
    const float r7 = r5 * r2;
    // -(2r) - r3 * 0.3333333 + r7 * -0.1079625 - r5 * 0.15, fused from the right; then + pi/2.
    const float series = fnmsubs(r, 2.0f, fnmsubs(r3, f32_bits(0x3EAAAAAAu),
                                                  fmsubs(r7, f32_bits(0xBDDD1B71u), r5 * f32_bits(0x3E19999Au))));
    return series + kHalfPiLow;
}

// atan2(y, x), approximately: the angle of (x, y) in radians, in [-pi, pi].
static float AngleOf(float x, float y) {
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    if (ay + ax == 0.0f) {
        return 0.0f;
    }
    const float ilength = 1.0f / std::sqrt(fmadds(y, y, x * x));  // 1 / sqrt(x*x + y*y)
    float angle;
    if (ax < ay) {
        angle = kHalfPi - ArcSine(ilength * ax);
    } else {
        angle = ArcSine(ilength * ay);
    }
    if (x < 0.0f) {
        angle = kPi - angle;
    }
    if (y < 0.0f) {
        angle = -angle;
    }
    return angle;
}

float vectoyaw(const vec3_t vec) {
    if (vec[1] == 0.0f && vec[0] == 0.0f) {
        return 0.0f;
    }
    float yaw = AngleOf(vec[0], vec[1]) * kRadiansToDegrees;
    if (yaw < 0.0f) {
        yaw += 360.0f;
    }
    return yaw;
}

float vectoyaw_signed(const vec3_t vec) {
    if (vec[1] == 0.0f && vec[0] == 0.0f) {
        return 0.0f;
    }
    return AngleOf(vec[0], vec[1]) * kRadiansToDegrees;
}

}  // namespace cod3
