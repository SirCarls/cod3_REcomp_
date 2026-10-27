// Call of Duty 3's q_math: angles and vectors, as the original source would
// read. [acct2-decomp]
//
// Written from the recompiled C++ (recomp/generated; names in q_shared.h) and
// the checked Rust ports (engine/crates/cod3_game/src/math/), not from Quake
// III's source (GPL; names and understanding only).
//
// Reading the arithmetic: the Xbox 360 compiler turned sums of products into
// fused multiply-adds (fmadds: one rounding instead of two). The source said
// `v[0]*v[0] + v[1]*v[1] + v[2]*v[2]`; to give the console's last bits on a
// PC the fused steps are written out with ppc::fmadds, each with the plain
// expression it came from next to it. Float to integer conversions are the
// console's fctiwz (ppc::fctiwz), which saturates where a C cast would be
// undefined.
#include <bit>
#include <cmath>
#include <cstdint>

#include "cod3/q_shared.h"

namespace cod3 {

using ppc::f32_bits;
using ppc::fmadds;
using ppc::fnmsubs;

// The game's angle factors, as the executable stores them.
static constexpr float kAngleToShort = f32_bits(0x43360B61u);  // 65536 / 360 = 182.04445
static constexpr float kShortToAngle = f32_bits(0x3BB40000u);  // 360 / 65536, exact

// Quake III's ANGLE2SHORT and SHORT2ANGLE (with the console's conversion).
static inline int ANGLE2SHORT(float x) { return ppc::fctiwz(static_cast<double>(x * kAngleToShort)) & 65535; }
static inline float SHORT2ANGLE(int x) { return static_cast<float>(x) * kShortToAngle; }

// ---- angles ---------------------------------------------------------------

float AngleNormalize180(float angle) {
    angle = SHORT2ANGLE(ANGLE2SHORT(angle));
    if (angle > 180.0f) {
        angle -= 360.0f;
    }
    return angle;
}

float AngleSubtract(float a1, float a2) {
    // The difference in single precision, its remainder by 360 in double (fmod).
    float a = static_cast<float>(std::fmod(static_cast<double>(a1 - a2), 360.0));
    if (a > 180.0f) {
        a -= 360.0f;
    }
    if (a < -180.0f) {
        a += 360.0f;
    }
    return a;
}

float AngleDelta(float angle1, float angle2) { return AngleNormalize180(angle1 - angle2); }

void AnglesSubtract(const vec3_t v1, const vec3_t v2, vec3_t v3) {
    v3[0] = AngleSubtract(v1[0], v2[0]);
    v3[1] = AngleSubtract(v1[1], v2[1]);
    v3[2] = AngleSubtract(v1[2], v2[2]);
}

// ---- vectors ----------------------------------------------------------------

// v[0]*v[0] + v[1]*v[1] + v[2]*v[2], as the console sums it: v[1]*v[1] first,
// then the other two squares fused in (x, then z).
static inline vec_t VectorLengthSquared(const vec3_t v) {
    return fmadds(v[2], v[2], fmadds(v[0], v[0], v[1] * v[1]));
}

vec_t VectorNormalize(vec3_t v) {
    const float length = std::sqrt(VectorLengthSquared(v));
    if (length) {
        const float ilength = 1.0f / length;
        v[0] *= ilength;
        v[1] *= ilength;
        v[2] *= ilength;
    }
    return length;
}

vec_t VectorNormalize2(const vec3_t v, vec3_t out) {
    const float length = std::sqrt(VectorLengthSquared(v));
    if (length) {
        const float ilength = 1.0f / length;
        out[0] = v[0] * ilength;
        out[1] = v[1] * ilength;
        out[2] = v[2] * ilength;
    } else {
        out[0] = out[1] = out[2] = 0.0f;
    }
    return length;
}

void VectorNormalizeFast(vec3_t v) {
    const float length_sq = VectorLengthSquared(v);
    // The fast inverse square root: a first guess from the float's bits (the
    // bits as a signed integer, halved with an arithmetic shift), then one
    // Newton step, guess * (1.5 - half * guess * guess), whose subtraction the
    // console fuses (fnmsubs).
    const float half = length_sq * 0.5f;
    const std::int32_t i = std::bit_cast<std::int32_t>(length_sq) >> 1;
    const float guess = std::bit_cast<float>(0x5F3759DFu - static_cast<std::uint32_t>(i));
    const float ilength = fnmsubs(half * guess, guess, 1.5f) * guess;
    v[0] *= ilength;
    v[1] *= ilength;
    v[2] *= ilength;
}

vec_t Vec2Normalize(vec2_t v) {
    const float length = std::sqrt(fmadds(v[0], v[0], v[1] * v[1]));  // v[0]*v[0] + v[1]*v[1]
    if (length) {
        const float ilength = 1.0f / length;
        v[0] *= ilength;
        v[1] *= ilength;
    }
    return length;
}

vec_t Vec2DistanceSq(const vec2_t p1, const vec2_t p2) {
    const float dx = p2[0] - p1[0];
    const float dy = p2[1] - p1[1];
    return fmadds(dy, dy, dx * dx);  // dx*dx + dy*dy
}

vec_t Vec2Distance(const vec2_t p1, const vec2_t p2) { return std::sqrt(Vec2DistanceSq(p1, p2)); }

vec_t Distance(const vec3_t p1, const vec3_t p2) {
    const float dy = p2[1] - p1[1];
    const float dz = p2[2] - p1[2];
    const float dx = p2[0] - p1[0];
    return std::sqrt(fmadds(dx, dx, fmadds(dz, dz, dy * dy)));  // dx*dx + dy*dy + dz*dz, y first
}

}  // namespace cod3
