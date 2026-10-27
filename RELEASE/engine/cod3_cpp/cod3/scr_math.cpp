// [acct2-decomp] The level scripts' math built-ins (scr_math.h), from the
// recompiled C++ (Broc_*) and the checked Rust ports
// (engine/crates/cod3_game/src/script/{builtins,random}.rs). The fused steps
// are written out with ppc::fmadds, the source expression next to each.
#include "cod3/scr_math.h"

#include <cmath>

#include "cod3/xbox_crt.h"

namespace cod3 {

using ppc::fmadds;

std::uint32_t brocRandomSeed = kBrocRandomInitialSeed;

float Broc_Abs(float x) { return std::fabs(x); }

float Broc_Sin(float x) { return static_cast<float>(xbox_crt::sin(static_cast<double>(x))); }

float Broc_Cos(float x) { return static_cast<float>(xbox_crt::cos(static_cast<double>(x))); }

float Broc_Sqrt(float x) { return std::sqrt(x); }

float Broc_LengthSquared(const vec3_t v) {
    return fmadds(v[2], v[2], fmadds(v[0], v[0], v[1] * v[1]));  // v[0]*v[0] + v[1]*v[1] + v[2]*v[2]
}

float Broc_Length(const vec3_t v) { return std::sqrt(Broc_LengthSquared(v)); }

float Broc_VectorDot(const vec3_t a, const vec3_t b) {
    return fmadds(a[0], b[0], fmadds(a[2], b[2], a[1] * b[1]));  // a[0]*b[0] + a[1]*b[1] + a[2]*b[2], y first
}

float Broc_DistanceSquared(const vec3_t p1, const vec3_t p2) {
    const float dy = p2[1] - p1[1];
    const float dz = p2[2] - p1[2];
    const float dx = p2[0] - p1[0];
    return fmadds(dx, dx, fmadds(dz, dz, dy * dy));  // dx*dx + dy*dy + dz*dz, y first
}

float Broc_Distance(const vec3_t p1, const vec3_t p2) { return std::sqrt(Broc_DistanceSquared(p1, p2)); }

int Broc_Closer(const vec3_t reference, const vec3_t a, const vec3_t b) {
    float d[3];
    d[1] = reference[1] - a[1];
    d[2] = reference[2] - a[2];
    d[0] = reference[0] - a[0];
    const float distance_a = fmadds(d[0], d[0], fmadds(d[2], d[2], d[1] * d[1]));
    d[1] = reference[1] - b[1];
    d[2] = reference[2] - b[2];
    d[0] = reference[0] - b[0];
    const float distance_b = fmadds(d[0], d[0], fmadds(d[2], d[2], d[1] * d[1]));
    return distance_a < distance_b;
}

void Broc_VectorNormalize(vec3_t out, const vec3_t v) {
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    const float length = std::sqrt(Broc_LengthSquared(out));
    if (length != 0.0f) {
        const float ilength = 1.0f / length;
        out[0] *= ilength;
        out[1] *= ilength;
        out[2] *= ilength;
    }
}

// ---- random numbers ---------------------------------------------------------
//
// Microsoft's rand constants, on a seed of the level scripts' own: each step
// gives 15 bits (0 to 32767). The products below wrap around like the
// console's 32-bit multiply (done in unsigned arithmetic, where C++ defines
// the wrap) and shift right keeping the sign.

static int Broc_NextRandom() {
    brocRandomSeed = brocRandomSeed * 214013u + 2531011u;
    return static_cast<int>(brocRandomSeed >> 17);
}

// a * b, a - b, a + b with the console's 32-bit wrap-around.
static int WrappingMultiply(int a, int b) {
    return static_cast<int>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
static int WrappingSubtract(int a, int b) {
    return static_cast<int>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
static int WrappingAdd(int a, int b) {
    return static_cast<int>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

int Broc_RandomInt(int max) { return WrappingMultiply(Broc_NextRandom(), max) >> 15; }  // (rand * max) >> 15

float Broc_RandomFloat(float max) {
    return (static_cast<float>(Broc_NextRandom()) * max) * (1.0f / 32768.0f);
}

int Broc_RandomIntRange(int min, int max) {
    const int value = Broc_NextRandom();
    if (max > min) {
        return WrappingAdd(WrappingMultiply(value, WrappingSubtract(max, min)) >> 15, min);
    }
    return WrappingAdd(WrappingMultiply(value, WrappingSubtract(min, max)) >> 15, max);
}

float Broc_RandomFloatRange(float min, float max) {
    float low;
    float span;
    if (max > min) {
        low = min;
        span = max - min;
    } else {
        low = max;
        span = min - max;
    }
    const float value = static_cast<float>(Broc_NextRandom());
    return fmadds(value * span, 1.0f / 32768.0f, low);  // value * span / 32768 + low
}

}  // namespace cod3
